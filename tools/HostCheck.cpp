// Loads a VST3 like a DAW does and reports the error / instantiation result.
// HPSAMPLER_HostCheck <plugin.vst3> [state.json] : with a state, the project is restored (sources load, pads render)
// and the time taken by every shutdown step is printed - used to find hangs on "remove plug-in" / DAW exit.
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>

static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    juce::ScopedJuceInitialiser_GUI init;
    juce::AudioPluginFormatManager fm;
    fm.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(found, juce::String(argv[1]));
    std::cout << "types found: " << found.size() << "\n";
    if (found.isEmpty()) return 2;
    juce::String err;
    auto inst = fm.createPluginInstance(*found[0], 48000.0, 512, err);
    std::cout << "instance: " << (inst ? "OK" : "FAILED") << " " << err << "\n";
    if (!inst) return 1;
    inst->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> b(2, 512);
    juce::MidiBuffer m;
    inst->processBlock(b, m);
    std::unique_ptr<juce::AudioProcessorEditor> ed(inst->createEditorIfNeeded());
    std::cout << "editor: " << (ed ? "OK" : "none") << "\n";
    if (ed != nullptr) // really on screen, so it paints (a host window)
    {
        ed->setTopLeftPosition(40, 40);
        ed->addToDesktop(juce::ComponentPeer::windowHasTitleBar);
        ed->setVisible(true);
        pump(800);
        std::cout << "editor shown and painted\n";
    }

    if (argc > 2)
    {
        const auto json = juce::File(argv[2]).loadFileAsString();
        inst->setStateInformation(json.toRawUTF8(), (int) json.getNumBytesAsUTF8());
        const int waitMs = argc > 3 ? juce::String(argv[3]).getIntValue() : 1500;
        // audio keeps running while the project restores, like in a host
        const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) waitMs;
        while (juce::Time::getMillisecondCounter() < until) { inst->processBlock(b, m); pump(10); }
        std::cout << "restored for " << waitMs << " ms\n";
    }
    auto t = juce::Time::getMillisecondCounterHiRes();
    auto step = [&](const char* what) { const auto now = juce::Time::getMillisecondCounterHiRes(); std::cout << what << ": " << (int) (now - t) << " ms\n"; t = now; };
    ed.reset();                 step("close editor");
    inst->releaseResources();   step("releaseResources");
    inst.reset();               step("delete instance");
    pump(200);                  step("message loop after delete");
    std::cout << "closed OK\n";
    return 0;
}
