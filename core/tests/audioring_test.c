/* audioring_test -- the rate-controlled ring holds its fill when producer and
 * consumer clocks differ by +-0.13% (the vsync-lock error) for ten minutes,
 * with no underrun and no overrun. Needs no machine, no SDL. */
#include <stdio.h>
#include <stdlib.h>

#include "audioring.h"

static audioring ring;

static int run(double producer_ratio)
{
    /* Consumer: 44100 Hz exactly. Producer: 735.93 samples per frame, frame
     * clock off by producer_ratio. Simulate in 1 ms ticks for 600 s. */
    double prod_acc = 0.0, frame_t = 0.0;
    const double frame_s = 59736.0 / 3579545.0 / producer_ratio;
    int16_t in[1024], out[64];
    long under = 0, over_max = 0;
    uint32_t minf = AR_FRAMES, maxf = 0;
    int t;

    audioring_reset(&ring);
    for (int i = 0; i < 1024; i++) in[i] = (int16_t)(i * 7);
    for (t = 0; t < 600 * 1000; t++) {
        double now = (t + 1) / 1000.0;
        /* producer frames due in this ms */
        while (frame_t <= now) {
            int n;
            prod_acc += 44100.0 * 59736.0 / 3579545.0;
            n = (int)prod_acc;
            prod_acc -= n;
            audioring_push(&ring, in, n);
            frame_t += frame_s;
        }
        /* consumer: 44.1 samples/ms */
        {
            static double cacc;
            int n;
            cacc += 44.1;
            n = (int)cacc;
            cacc -= n;
            if (audioring_pull(&ring, out, n) < n) under++;
        }
        if (t > 5000) {
            uint32_t f = audioring_fill(&ring);
            if (f < minf) minf = f;
            if (f > maxf) maxf = f;
        }
    }
    printf("ratio %.4f: fill %u..%u, underruns %ld\n", producer_ratio, minf,
           maxf, under);
    (void)over_max;
    if (under > 0) return 1;
    /* fill is a sawtooth one frame (~736) wide; it must stay clear of both ends */
    if (minf < 300 || maxf > AR_TARGET * 2) return 1;
    return 0;
}

int main(void)
{
    int bad = 0;
    bad |= run(1.0);
    bad |= run(1.0013);
    bad |= run(0.9987);
    puts(bad ? "FAIL" : "PASS");
    return bad;
}
