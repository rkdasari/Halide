// Halide tutorial lesson 12.

// This lesson demonstrates how to use Halide to run code on a GPU.

// On linux, you can compile and run it like so:
// g++ lesson_12*.cpp -g -I ../include -L ../bin -lHalide -lpthread -ldl -o lesson_12
// LD_LIBRARY_PATH=../bin ./lesson_12

// On os x:
// g++ lesson_12*.cpp -g -I ../include -L ../bin -lHalide -o lesson_12
// DYLD_LIBRARY_PATH=../bin ./lesson_12

#include <Halide.h>
#include <stdio.h>
using namespace Halide;

// Include some support code for loading pngs.
#include "image_io.h"

// Include a clock to do performance testing.
#include "clock.h"

// Define some Vars to use.
Var x, y, c, i;

// We're going to want to schedule a pipeline in several ways, so we
// define the pipeline in a class so that we can recreate it several
// times with different schedules.
class Pipeline {
public:
    Func lut, padded, padded16, sharpen, curved;
    Image<uint8_t> input;

    Pipeline(Image<uint8_t> in) : input(in) {
        // For this lesson, we'll use a two-stage pipeline that sharpens
        // and then applies a look-up-table (LUT).

        // First we'll define the LUT. It will be a gamma curve.

        lut(i) = cast<uint8_t>(clamp(pow(i / 255.0f, 1.2f) * 255.0f, 0, 255));

        // Augment the input with a boundary condition.
        padded(x, y, c) = input(clamp(x, 0, input.width()-1),
                                clamp(y, 0, input.height()-1), c);

        // Cast it to 16-bit to do the math.
        padded16(x, y, c) = cast<uint16_t>(padded(x, y, c));

        // Next we sharpen it with a five-tap filter.
        sharpen(x, y, c) = (padded16(x, y, c) * 2-
                            (padded16(x - 1, y, c) +
                             padded16(x, y - 1, c) +
                             padded16(x + 1, y, c) +
                             padded16(x, y + 1, c)) / 4);

        // Then apply the LUT.
        curved(x, y, c) = lut(sharpen(x, y, c));
    }

    // Now we define methods that give our pipeline several different
    // schedules.
    void schedule_for_cpu() {
        // Compute the look-up-table ahead of time.
        lut.compute_root();

        // Compute color channels innermost. Promise that there will
        // be three of them and unroll across them.
        curved.reorder(c, x, y)
              .bound(c, 0, 3)
              .unroll(c);

        // Look-up-tables don't vectorize well, so just parallelize
        // curved in slices of 16 scanlines.
        Var yo, yi;
        curved.split(y, yo, yi, 16)
              .parallel(yo);

        // Compute sharpen as needed per scanline of curved, reusing
        // previous values computed within the same strip of 16
        // scanlines.
        sharpen.store_at(curved, yo)
               .compute_at(curved, yi);

        // Vectorize the sharpen. It's 16-bit so we'll vectorize it 8-wide.
        sharpen.vectorize(x, 8);

        // Compute the padded input at the same granularity as the
        // sharpen. We'll leave the cast to 16-bit inlined into
        // sharpen.
        padded.store_at(curved, yo)
              .compute_at(curved, yi);

        // Also vectorize the padding. It's 8-bit, so we'll vectorize
        // 16-wide.
        padded.vectorize(x, 16);

        // JIT-compile the pipeline for the CPU.
        curved.compile_jit();
    }

    // Now a schedule that uses CUDA or OpenCL.
    void schedule_for_gpu() {
        // We make the decision about whether to use the GPU for each
        // Func independently. If you have one Func computed on the
        // CPU, and the next computed on the GPU, Halide will do the
        // copy-to-gpu under the hood. For this pipeline, there's no
        // reason to use the CPU for any of the stages. Halide will
        // copy the input image to the GPU the first time we run the
        // pipeline, and leave it there to reuse on subsequent runs.

        // As before, we'll compute the LUT once at the start of the
        // pipeline.
        lut.compute_root();

        // Let's compute the look-up-table using the GPU in 16-wide
        // one-dimensional thread blocks. First we split the index
        // into blocks of size 16:
        Var block, thread;
        lut.split(i, block, thread, 16);
        // Then we tell cuda that our Vars 'block' and 'thread'
        // correspond to CUDA's notions of blocks and threads, or
        // OpenCL's notions of thread groups and threads.
        lut.gpu_blocks(block)
           .gpu_threads(thread);

        // This is a very common scheduling pattern on the GPU, so
        // there's a shorthand for it:

        // lut.gpu_tile(i, 16);

        // Func::gpu_tile method is similar to Func::tile, except that
        // it also specifies that the tile coordinates correspond to
        // GPU blocks, and the coordinates within each tile correspond
        // to GPU threads.

        // Compute color channels innermost. Promise that there will
        // be three of them and unroll across them.
        curved.reorder(c, x, y)
              .bound(c, 0, 3)
              .unroll(c);

        // Compute curved in 2D 8x8 tiles using the GPU.
        curved.gpu_tile(x, y, 8, 8);

        // This is equivalent to:
        // curved.tile(x, y, xo, yo, xi, yi, 8, 8)
        //       .gpu_blocks(xo, yo)
        //       .gpu_threads(xi, yi);

        // We'll leave sharpen as inlined into curved.

        // Compute the padded input as needed per GPU block, storing the
        // intermediate result in shared memory. Var::gpu_blocks, and
        // Var::gpu_threads exist to help you schedule producers within
        // GPU threads and blocks.
        padded.compute_at(curved, Var::gpu_blocks());

        // Use the GPU threads for the x and y coordinates of the
        // padded input.
        padded.gpu_threads(x, y);

        // JIT-compile the pipeline for the GPU. CUDA or OpenCL are
        // not enabled by default. We have to construct a Target
        // object, enable one of them, and then pass that target
        // object to compile_jit. Otherwise your CPU will very slowly
        // pretend it's a GPU, and use one thread per output pixel.

        // Start with a target suitable for the machine you're running
        // this on.
        Target target = get_host_target();

        // Then enable OpenCL or CUDA.

        // We'll enable OpenCL here, because it tends to give better
        // performance than CUDA, even with NVidia's drivers, because
        // NVidia's open source LLVM backend doesn't seem to do all
        // the same optimizations their proprietary compiler does.
        target.features |= Target::OpenCL;

        // Uncomment the next line and comment out the line above to
        // try CUDA instead.
        // target.features |= Target::CUDA;

        // If you want to see all of the OpenCL or CUDA API calls done
        // by the pipeline, you can also enable the GPUDebug
        // flag. This is helpful for figuring out which stages are
        // slow, or when CPU -> GPU copies happen. It hurts
        // performance though, so we'll leave it commented out.
        //target.features |= Target::GPUDebug;

        curved.compile_jit(target);
    }

    void test_performance() {
        // Test the performance of the scheduled Pipeline.

        // If we realize curved into a Halide::Image, that will
        // unfairly penalize GPU performance by including a GPU->CPU
        // copy. Halide::Image objects always exist on the CPU.

        // Halide::Buffer, however, represents a buffer that may
        // exist on either CPU or GPU or both. Constructing an
        // Image using a Buffer forces the copy-back from the GPU.
        Buffer output(UInt(8), input.width(), input.height(), input.channels());

        // Take the best of several runs.
        double best_time;
        for (int i = 0; i < 20; i++) {
            double t1 = current_time();
            curved.realize(output);
            double t2 = current_time();

            double elapsed = t2 - t1;
            if (i == 0 || elapsed < best_time) {
                best_time = elapsed;
            }
        }

        printf("%1.4f milliseconds\n", best_time);
    }

    void test_correctness(Image<uint8_t> reference_output) {
        Image<uint8_t> output = curved.realize(input.width(), input.height(), input.channels());

        // Check against the reference output.
        for (int c = 0; c < input.channels(); c++) {
            for (int y = 0; y < input.height(); y++) {
                for (int x = 0; x < input.width(); x++) {
                    if (output(x, y, c) != reference_output(x, y, c)) {
                        printf("Mismatch between output (%d) and "
                               "reference output (%d) at %d, %d, %d\n",
                               output(x, y, c),
                               reference_output(x, y, c),
                               x, y, c);
                    }
                }
            }
        }

    }
};

int main(int argc, char **argv) {
    // Load an input image.
    Image<uint8_t> input = load<uint8_t>("images/rgb.png");

    // Allocated an image that will store the correct output
    Image<uint8_t> reference_output(input.width(), input.height(), input.channels());

    printf("Testing performance on CPU:\n");
    Pipeline p1(input);
    p1.schedule_for_cpu();
    p1.test_performance();
    p1.curved.realize(reference_output);

    printf("Testing performance on GPU:\n");
    Pipeline p2(input);
    p2.schedule_for_gpu();
    p2.test_performance();
    p2.test_correctness(reference_output);

    return 0;
}