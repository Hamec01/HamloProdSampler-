// Step-by-step VST3 instantiation with the SDK interfaces only (no JUCE host), printing each result.
#include <windows.h>
#define INIT_CLASS_IID
#include <cstdio>
#include <cstring>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
using namespace Steinberg;
typedef IPluginFactory* (PLUGIN_API *GetFactoryProc)();
typedef bool (PLUGIN_API *InitDllProc)();

int main(int argc, char** argv)
{
    if (argc < 2) return 1;
    HMODULE m = LoadLibraryA(argv[1]);
    printf("LoadLibrary: %p (err %lu)\n", (void*) m, GetLastError());
    if (!m) return 1;
    if (auto init = (InitDllProc) GetProcAddress(m, "InitDll")) printf("InitDll: %d\n", (int) init());
    auto gf = (GetFactoryProc) GetProcAddress(m, "GetPluginFactory");
    IPluginFactory* f = gf ? gf() : nullptr;
    printf("factory: %p\n", (void*) f);
    if (!f) return 2;
    const int n = f->countClasses();
    for (int i = 0; i < n; ++i)
    {
        PClassInfo ci;
        f->getClassInfo(i, &ci);
        printf("class %d: %s [%s] cid ", i, ci.name, ci.category);
        for (int b = 0; b < 16; ++b) printf("%02X", (unsigned char) ci.cid[b]);
        printf("\n");
        if (strcmp(ci.category, kVstAudioEffectClass) != 0) continue;
        Vst::IComponent* comp = nullptr;
        printf("  createInstance(IComponent): %d\n", (int) f->createInstance(ci.cid, Vst::IComponent::iid, (void**) &comp));
        if (!comp) continue;
        printf("  initialize: %d\n", (int) comp->initialize(nullptr));
        printf("  buses: in %d out %d\n", (int) comp->getBusCount(Vst::kAudio, Vst::kInput), (int) comp->getBusCount(Vst::kAudio, Vst::kOutput));
        TUID ctrlCid;
        printf("  getControllerClassId: %d\n", (int) comp->getControllerClassId(ctrlCid));
        Vst::IEditController* ec = nullptr;
        if (comp->queryInterface(Vst::IEditController::iid, (void**) &ec) == kResultOk && ec)
        {
            printf("  single-component controller\n");
            ec->release();
        }
        else
        {
            printf("  createInstance(controller): %d\n", (int) f->createInstance(ctrlCid, Vst::IEditController::iid, (void**) &ec));
            if (ec)
            {
                printf("  controller initialize: %d, params %d\n", (int) ec->initialize(nullptr), (int) ec->getParameterCount());
                ec->terminate();
                ec->release();
            }
        }
        Vst::IAudioProcessor* ap = nullptr;
        comp->queryInterface(Vst::IAudioProcessor::iid, (void**) &ap);
        printf("  IAudioProcessor: %p\n", (void*) ap);
        if (ap)
        {
            Vst::SpeakerArrangement out = Vst::SpeakerArr::kStereo;
            printf("  setBusArrangements(0 in, stereo out): %d\n", (int) ap->setBusArrangements(nullptr, 0, &out, 1));
            Vst::ProcessSetup ps { Vst::kRealtime, Vst::kSample32, 512, 48000.0 };
            printf("  setupProcessing: %d\n", (int) ap->setupProcessing(ps));
            printf("  activateBus out: %d\n", (int) comp->activateBus(Vst::kAudio, Vst::kOutput, 0, true));
            printf("  setActive: %d\n", (int) comp->setActive(true));
            comp->setActive(false);
            ap->release();
        }
        comp->terminate();
        comp->release();
    }
    return 0;
}
