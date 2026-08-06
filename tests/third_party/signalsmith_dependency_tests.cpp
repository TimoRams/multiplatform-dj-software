#include <signalsmith-dsp/delay.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

int main()
{
    signalsmith::delay::Delay<float> delay(1024);
    delay.write(0.5f);

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetCheaper(2, 48000.0f);

    return delay.read(0.0f) == 0.5f ? 0 : 1;
}
