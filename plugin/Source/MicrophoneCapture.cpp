#include "MicrophoneCapture.h"

juce::String MicrophoneCapture::start() {
  cancel(); error_.clear();
  error_ = devices_.initialiseWithDefaultDevices(1,0);
  if (error_.isNotEmpty()) {devices_.closeAudioDevice();return error_;}
  auto* device = devices_.getCurrentAudioDevice();
  if (!device || device->getActiveInputChannels().isZero()) {
    error_="No microphone input is available";devices_.closeAudioDevice();return error_;
  }
  rate_=device->getCurrentSampleRate();
  recording_.setSize(1,static_cast<int>(rate_*60));
  position_=0;full_=false;level_=0;active_=true;
  devices_.addAudioCallback(this);
  return {};
}
void MicrophoneCapture::closeInput() {
  active_=false;devices_.removeAudioCallback(this);devices_.closeAudioDevice();
}
void MicrophoneCapture::cancel() {
  closeInput();position_=0;full_=false;level_=0;recording_.setSize(0,0);
}
void MicrophoneCapture::audioDeviceIOCallbackWithContext(const float* const* input, int channels,
    float* const* output, int outputs, int count, const juce::AudioIODeviceCallbackContext&) {
  for(int c=0;c<outputs;++c) if(output[c]) juce::FloatVectorOperations::clear(output[c],count);
  if(!active_.load() || channels<1) return;
  const int from=position_.load(),take=std::min(count,recording_.getNumSamples()-from);
  float peak=0;
  for(int i=0;i<take;++i) {
    float value=0;int valid=0;
    for(int c=0;c<channels;++c) if(input[c]) {value+=input[c][i];++valid;}
    value/=static_cast<float>(std::max(1,valid));
    recording_.setSample(0,from+i,value);peak=std::max(peak,std::fabs(value));
  }
  position_=from+take;level_=peak;
  if(from+take==recording_.getNumSamples()) {full_=true;active_=false;}
}
juce::MemoryBlock MicrophoneCapture::encode(const juce::AudioBuffer<float>& audio,int samples,double rate) {
  juce::MemoryBlock data;
  if(samples<=0 || samples>audio.getNumSamples() || audio.getNumChannels()<1 || !std::isfinite(rate) || rate<=0) return data;
  auto stream=std::make_unique<juce::MemoryOutputStream>(data,false);
  juce::WavAudioFormat format;
  std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(stream.get(),rate,1,16,{},0));
  if(!writer) return {};
  stream.release();
  if(!writer->writeFromAudioSampleBuffer(audio,0,samples)) return {};
  writer.reset();return data;
}
juce::MemoryBlock MicrophoneCapture::stop() {
  closeInput();auto data=encode(recording_,position_.load(),rate_);full_=false;position_=0;level_=0;recording_.setSize(0,0);return data;
}
juce::var MicrophoneCapture::status() const {
  auto* o=new juce::DynamicObject();
  o->setProperty("recording",active_.load());o->setProperty("full",full_.load());
  o->setProperty("seconds",position_.load()/rate_);o->setProperty("level",level_.load());
  o->setProperty("error",error_);return juce::var(o);
}
