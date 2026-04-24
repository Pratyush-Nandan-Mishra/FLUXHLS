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

#pragma HLS INTERFACE m_axi port=input   // input feature map read from DDR via AXI4-Master
#pragma HLS INTERFACE m_axi port=kernel  // convolution kernel read from DDR via AXI4-Master
#pragma HLS INTERFACE m_axi port=output  // output feature map written to DDR via AXI4-Master
#pragma HLS INTERFACE s_axilite port=return  // ap_return on AXI4-Lite control bus

#define IH 32               // input image height in pixels
#define IW 32               // input image width in pixels
#define KH  3               // kernel height: number of kernel rows
#define KW  3               // kernel width: number of kernel columns
#define OH (IH - KH + 1)    // output height: valid-padding convolution shrinks by kernel_size - 1
#define OW (IW - KW + 1)    // output width: valid-padding convolution shrinks by kernel_size - 1

void conv2d(float input[IH][IW], float kernel[KH][KW], float output[OH][OW]) {  // 2D valid-padding convolution kernel
    for (int oh = 0; oh < OH; oh++) {              // outer loop: iterate over output image rows
        for (int ow = 0; ow < OW; ow++) {          // second loop: iterate over output image columns
            float sum = 0.0f;                       // accumulator: reset for each output pixel
            for (int kh = 0; kh < KH; kh++) {      // third loop: iterate over kernel rows
                for (int kw = 0; kw < KW; kw++) {  // innermost loop: iterate over kernel columns
#pragma HLS PIPELINE II=1                           //** pipeline kw loop: one multiply-accumulate per clock cycle
                    sum += input[oh + kh][ow + kw] * kernel[kh][kw];  // MAC: accumulate one kernel tap into sum
                }
            }
            output[oh][ow] = sum;                   // write the completed convolution result for this output pixel
        }
    }
}
