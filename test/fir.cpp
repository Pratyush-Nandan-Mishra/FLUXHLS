// SAMPLE 2 — FIR Filter (shows UNROLL + local array partitioning)
//
// What HLS does here:
//   - UNROLL factor=4: physically duplicates the inner loop 4 times in hardware.
//     Instead of 1 multiplier doing 16 ops sequentially, you get 4 multipliers
//     doing 4 ops in parallel — 4x throughput.
//   - ARRAY_PARTITION on 'coeffs': splits the coefficient array into 4 separate
//     BRAMs (or registers) so all 4 unrolled iterations can read simultaneously.
//     Without this, all 4 copies would fight over one memory port and the unroll
//     would gain nothing.
//   - The outer loop is pipelined: a new input sample starts every clock cycle.

#pragma HLS INTERFACE m_axi port=in_signal
#pragma HLS INTERFACE m_axi port=out_signal
#pragma HLS INTERFACE s_axilite port=return

#define TAPS 16

void fir_filter(float *in_signal, float *out_signal, int length) {
    float coeffs[TAPS] = {
        0.1f, 0.2f, 0.3f, 0.4f, 0.4f, 0.3f, 0.2f, 0.1f,
        0.1f, 0.2f, 0.3f, 0.4f, 0.4f, 0.3f, 0.2f, 0.1f
    };
#pragma HLS ARRAY_PARTITION variable=coeffs type=cyclic factor=4

    for (int i = 0; i < length; i++) {
#pragma HLS PIPELINE II=1
        float acc = 0.0f;
        for (int t = 0; t < TAPS; t++) {
#pragma HLS UNROLL factor=4
            acc += in_signal[i - t] * coeffs[t];
        }
        out_signal[i] = acc;
    }
}
