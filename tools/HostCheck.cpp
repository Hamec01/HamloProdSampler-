// Loads a VST3 like a DAW does and reports the error / instantiation result.
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>
int main(int argc, char** argv)
{
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
    juce::AudioBuffer<float> b(2, 512); juce::MidiBuffer m;
    inst->processBlock(b, m);
    std::unique_ptr<juce::AudioProcessorEditor> ed(inst->createEditorIfNeeded());
    std::cout << "editor: " << (ed ? "OK" : "none") << "\n";
    ed.reset();
    inst.reset();
    std::cout << "closed OK\n";
    return 0;
}
