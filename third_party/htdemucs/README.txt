HTDemucs v4 ("htdemucs", Meta, MIT license: LICENSE).
htdemucs.onnx = tools/stems/export_htdemucs_onnx.py, then tools/stems/shrink_fp16.py
(weights stored fp16, lossless: the published checkpoint is fp16). Inputs mix[1,2,343980], spec[1,4,2048,336];
outputs freq[1,4,4,2048,336], time[1,4,2,343980]. Source order: drums, bass, other, vocals.
