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

#pragma HLS INTERFACE m_axi port=in_signal   // input sample stream read from DDR via AXI4-Master
#pragma HLS INTERFACE m_axi port=out_signal  // output sample stream written to DDR via AXI4-Master
#pragma HLS INTERFACE s_axilite port=return  // ap_return on the AXI4-Lite control bus

#define TAPS 16   // number of FIR filter taps (filter order = TAPS - 1)

void fir_filter(float *in_signal, float *out_signal, int length) {   // FIR filter kernel: convolve in_signal with coeffs
    float coeffs[TAPS] = {                   // local coefficient array — synthesised to BRAM or registers
        0.1f, 0.2f, 0.3f, 0.4f, 0.4f, 0.3f, 0.2f, 0.1f,  // symmetric first half of the impulse response
        0.1f, 0.2f, 0.3f, 0.4f, 0.4f, 0.3f, 0.2f, 0.1f   // symmetric second half of the impulse response
    };
#pragma HLS ARRAY_PARTITION variable=coeffs type=cyclic factor=4  //** split coeffs into 4 cyclic BRAM banks so all 4 unrolled copies read simultaneously

    for (int i = 0; i < length; i++) {       //** outer pipeline loop: produces one output sample per iteration
#pragma HLS PIPELINE II=1                    // target one new output sample started per clock cycle
        float acc = 0.0f;                    // accumulator register: reset to zero at the start of each i-iteration
        for (int t = 0; t < TAPS; t++) {     // inner loop: accumulate contributions from all 16 taps
#pragma HLS UNROLL factor=4                  //** unroll 4x: synthesise 4 parallel multipliers to compute 4 taps per cycle
            acc += in_signal[i - t] * coeffs[t];  // multiply tap t of the input by the corresponding coefficient
        }
        out_signal[i] = acc;                 // write the completed filtered output sample to DDR
    }
}
