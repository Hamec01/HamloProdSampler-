"""Export HTDemucs v4 (4 stems) to ONNX for HPStems.exe.

The STFT / iSTFT stay outside the graph (ONNX has no good complex iSTFT); HPStems does them in C++.
Graph inputs / outputs (fixed shape, one 7.8 s segment at 44.1 kHz):
    mix   [1, 2, 343980]        normalised stereo segment
    spec  [1, 4, 2048, 336]     cac spectrogram of `mix` (re/im as channels: L.re, L.im, R.re, R.im)
 -> freq  [1, 4, 4, 2048, 336]  per source cac spectrogram (drums, bass, other, vocals)
 -> time  [1, 4, 2, 343980]     per source time-branch signal
Stem = iSTFT(freq) + time.

Usage:  python -I export_htdemucs_onnx.py <out.onnx> [--check]
Needs: torch, demucs 4.x, onnx.
"""
import math
import sys

import torch
from torch import nn

from demucs.pretrained import get_model
from demucs.htdemucs import HTDemucs


class Core(nn.Module):
    """HTDemucs.forward without _spec / _mask / _ispec (same code, same order)."""

    def __init__(self, m: HTDemucs):
        super().__init__()
        self.m = m

    def forward(self, mix, spec):
        m = self.m
        x = spec
        B, C, Fq, T = x.shape
        mean = x.mean(dim=(1, 2, 3), keepdim=True)
        std = x.std(dim=(1, 2, 3), keepdim=True)
        x = (x - mean) / (1e-5 + std)
        xt = mix
        meant = xt.mean(dim=(1, 2), keepdim=True)
        stdt = xt.std(dim=(1, 2), keepdim=True)
        xt = (xt - meant) / (1e-5 + stdt)

        saved, saved_t, lengths, lengths_t = [], [], [], []
        for idx, encode in enumerate(m.encoder):
            lengths.append(x.shape[-1])
            inject = None
            if idx < len(m.tencoder):
                lengths_t.append(xt.shape[-1])
                tenc = m.tencoder[idx]
                xt = tenc(xt)
                if not tenc.empty:
                    saved_t.append(xt)
                else:
                    inject = xt
            x = encode(x, inject)
            if idx == 0 and m.freq_emb is not None:
                frs = torch.arange(x.shape[-2], device=x.device)
                emb = m.freq_emb(frs).t()[None, :, :, None].expand_as(x)
                x = x + m.freq_emb_scale * emb
            saved.append(x)
        if m.crosstransformer:
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_upsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_upsampler_t(xt)
            x, xt = m.crosstransformer(x, xt)
            if m.bottom_channels:
                b, c, f, t = x.shape
                x = x.reshape(b, c, f * t)
                x = m.channel_downsampler(x)
                x = x.reshape(b, -1, f, t)
                xt = m.channel_downsampler_t(xt)

        for idx, decode in enumerate(m.decoder):
            skip = saved.pop(-1)
            x, pre = decode(x, skip, lengths.pop(-1))
            offset = m.depth - len(m.tdecoder)
            if idx >= offset:
                tdec = m.tdecoder[idx - offset]
                length_t = lengths_t.pop(-1)
                if tdec.empty:
                    pre = pre[:, :, 0]
                    xt, _ = tdec(pre, None, length_t)
                else:
                    skip = saved_t.pop(-1)
                    xt, _ = tdec(xt, skip, length_t)

        S = len(m.sources)
        x = x.view(B, S, -1, Fq, T)
        x = x * std[:, None] + mean[:, None]
        L = mix.shape[-1]
        xt = xt.view(B, S, -1, L)
        xt = xt * stdt[:, None] + meant[:, None]
        return x, xt


def main():
    torch.backends.mha.set_fastpath_enabled(False)  # the fused MHA kernel has no ONNX export
    out = sys.argv[1]
    check = "--check" in sys.argv
    bag = get_model("htdemucs")
    m = bag.models[0]
    m.eval()
    L = int(m.segment * m.samplerate)
    T = int(math.ceil(L / m.hop_length))
    print("segment", L, "frames", T, "sources", m.sources, flush=True)

    core = Core(m).eval()
    mix = torch.randn(1, 2, L) * 0.3
    z = m._spec(mix)
    spec = m._magnitude(z)

    if check:
        with torch.no_grad():
            ref = m(mix)
            fx, tx = core(mix, spec)
            zout = m._mask(z, fx)
            mine = m._ispec(zout, L) + tx
        print("core vs model max abs diff:", (ref - mine).abs().max().item(), flush=True)

    torch.onnx.export(core, (mix, spec), out, input_names=["mix", "spec"], output_names=["freq", "time"],
                      opset_version=17, dynamo=False, do_constant_folding=True)
    print("written", out, flush=True)


if __name__ == "__main__":
    with torch.no_grad():
        main()
