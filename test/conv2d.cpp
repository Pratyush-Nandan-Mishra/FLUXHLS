// SAMPLE 3 — 2-D Convolution (shows deep loop nesting + multiple pragmas)
//
// What HLS does here:
//   - The two outer loops (oh, ow) iterate over the output image pixels.
//   - The two inner loops (kh, kw) iterate over the 3x3 kernel.
//   - PIPELINE on the innermost loop (kw): one MAC per cycle.
//   - HLS calculates: Initiation Interval = max(RecMII, ResMII)
//       RecMII = loop-carried dependency on 'sum' (accumulator) = 1 cycle
//       ResMII = 1 MAC / (1 DSP48 available per cycle) = 1 cycle
//     So II=1 is achievable — the kernel fully pipelines.
//   - Latency for one output pixel = kh * kw = 9 clock cycles.
//   - Total latency ≈ OH * OW * 9 cycles.
//
// Key insight: HLS can achieve the same throughput as a hand-written RTL
// engineer would produce, but from a simple nested loop in C.

#pragma HLS INTERFACE m_axi port=input
#pragma HLS INTERFACE m_axi port=kernel
#pragma HLS INTERFACE m_axi port=output
#pragma HLS INTERFACE s_axilite port=return

#define IH 32
#define IW 32
#define KH  3
#define KW  3
#define OH (IH - KH + 1)
#define OW (IW - KW + 1)

void conv2d(float input[IH][IW], float kernel[KH][KW], float output[OH][OW]) {
    for (int oh = 0; oh < OH; oh++) {
        for (int ow = 0; ow < OW; ow++) {
            float sum = 0.0f;
            for (int kh = 0; kh < KH; kh++) {
                for (int kw = 0; kw < KW; kw++) {
#pragma HLS PIPELINE II=1
                    sum += input[oh + kh][ow + kw] * kernel[kh][kw];
                }
            }
            output[oh][ow] = sum;
        }
    }
}
