// OpenCL.cpp : Defines the entry point for the console application.
//

#include <iostream>
#include <ctime>
#include <limits>
#include <cmath>

#include <CL/cl.h>

#include "basic.hpp"
#include "cmdoptions.hpp"
#include "oclobject.hpp"

using namespace std;


template <class T>
bool checkValidity (
    const T* A,     // left input matrix, column-major
    const T* B,     // right input matrix, column-major or row-major depending on Btransposed argument
    const T* C,     // output matrix, column-major
    size_t size,    // number of column in each of the matrices
    size_t ldabc,   // row stride: the number of T elements in one row (including optional padding)
    bool Btransposed,
    T alpha, T beta // coefficient to compute
)
{
    cout << "Validate output..." << flush;

    // Btransposed == false, lstride = 1
    size_t lstride = Btransposed ? ldabc : 1;
    size_t jstride = Btransposed ? 1 : ldabc;

    // Estimate error tolerance for a given type T and relying on the fact
    // that initial matrix values are from [0, 1]
    T max_value = 1;
    T error_tol = alpha * max_value * max_value * T(2) * size * numeric_limits<T>::epsilon();

    for(size_t i = 0; i < size; ++i)
    {
        for(size_t j = 0; j < size; ++j)
        {
            // compute golden value for c[i][j] element
            T accum = 0;
            for(size_t l = 0; l < size; ++l)
            {
                accum += A[l*ldabc + i] * B[l*lstride + j*jstride];
            }

            T golden = alpha*accum;

            if(abs(C[j*ldabc+i] - golden) > error_tol)
            {
                cerr << " FAILED!!!\nGolden" << "[" << i << ", " << j << "] = "
                     << golden << ", calculated" << "[" << i << ", " << j << "] = "
                     << C[j*ldabc+i] << "\n";
                return false;
            }
        }
    }

    std::cout << " PASSED\n";
    return true;
}


// The main GEMM function with all application specific
// OpenCL host side code.
template <typename T>
void gemm (
    CmdParserGEMM& cmdparser,
    OpenCLBasic& oclobjects,
    OpenCLProgramOneKernel& executable
)
{
    // -----------------------------------------------------------------------
    // Calculating, allocating and initializing host-side memory
    // -----------------------------------------------------------------------

    // Query for necessary alignment
    size_t alignment = requiredOpenCLAlignment(oclobjects.device);
    assert(alignment >= sizeof(T)); // must be
    assert((alignment & (alignment - 1)) == 0); // test for power of 2

    cmdparser.validateParameters(oclobjects, executable, sizeof(T), alignment);

    size_t size = cmdparser.size.getValue();

    cout
        << "Running gemm_" << cmdparser.kernel.getValue()
        << " kernel with matrix size: " << size << "x" << size << "\n";

    // Ensures that each matrix memory row is aligned
    size_t stride = (size*sizeof(T) + alignment - 1) & ~(alignment - 1);
    cout << "Memory row stride to ensure necessary alignment: " << stride << " bytes\n";
    // calculate row stride in elements of T
    stride /= sizeof(T);
    assert(size <= stride);

    if(stride/sizeof(T) > size_t(numeric_limits<cl_int>::max()))
    {
        throw Error(
            "Memory row stride in elements " + to_str(stride/sizeof(T)) +
            " cannot be represented as type int, which can be maximum " +
            to_str(numeric_limits<cl_int>::max()) + "."
        );
    }

    size_t matrix_memory_size = size*stride*sizeof(T);
    cout << "Size of memory region for one matrix: " << matrix_memory_size << " bytes\n";

    // Allocate aligned memory for matrices to use them in
    // buffers with CL_MEM_USE_HOST_PTR.
    // OpenCLDeviceAndHostMemory is used just for
    // convenient resource deallocation:
    // a pair of pointer and cl_mem object; cl_mem object is
    // be creater later.

    OpenCLDeviceAndHostMemory<T> matrix_A;
    matrix_A.host = (T*)aligned_malloc(matrix_memory_size, alignment);

    OpenCLDeviceAndHostMemory<T> matrix_B;
    matrix_B.host = (T*)aligned_malloc(matrix_memory_size, alignment);

    OpenCLDeviceAndHostMemory<T> matrix_C;
    matrix_C.host = (T*)aligned_malloc(matrix_memory_size, alignment);

    // Initialize matrices row by row.
    for(size_t i = 0; i < size; ++i)
    {
        T* row_A = matrix_A.host + i*stride;
        T* row_B = matrix_B.host + i*stride;
        T* row_C = matrix_C.host + i*stride;

        // Fill the rows with random values from range [0, 1]
        fill_rand_uniform_01(row_A, size);
        fill_rand_uniform_01(row_B, size);

        // To simplify validation a bit, we initialize C matrix with all zeros.
        // It should not affect performance, which should be identical to
        // the general case.
        std::fill(row_C, row_C + size, T(0));
    }

    // -----------------------------------------------------------------------
    // Allocating device-side resources for matrices
    // -----------------------------------------------------------------------

    cl_int err = 0; // OpenCL error code

    // Create OpenCL buffers for the matrices based on allocated memory regions
    // Create buffers with CL_MEM_USE_HOST_PTR to minimize copying and
    // model situation when matrices are hosted by some native library that
    // uses OpenCL to accelerate calculations.

    matrix_A.device = clCreateBuffer(
        oclobjects.context,
        CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
        matrix_memory_size,
        matrix_A.host,
        &err
    );
    SAMPLE_CHECK_ERRORS(err);

    matrix_B.device = clCreateBuffer(
        oclobjects.context,
        CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
        matrix_memory_size,
        matrix_B.host,
        &err
    );
    SAMPLE_CHECK_ERRORS(err);

    matrix_C.device = clCreateBuffer(
        oclobjects.context,
        CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
        matrix_memory_size,
        matrix_C.host,
        &err
    );
    SAMPLE_CHECK_ERRORS(err);

    T alpha = rand_uniform_01<T>();
    T beta = rand_uniform_01<T>();
    cout << "Using alpha = " << alpha << " and beta = " << beta << "\n";
    cl_int cl_size = static_cast<int>(size);  // kernel requires int value
    cl_int ldabc = static_cast<int>(stride);  // kernel requires int value

    // -----------------------------------------------------------------------
    // Setting kernel arguments
    // -----------------------------------------------------------------------

    err = clSetKernelArg(executable.kernel, 0, sizeof(cl_mem), &matrix_A.device);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 1, sizeof(cl_int), &ldabc);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 2, sizeof(cl_mem), &matrix_B.device);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 3, sizeof(cl_int), &ldabc);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 4, sizeof(cl_mem), &matrix_C.device);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 5, sizeof(cl_int), &ldabc);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 6, sizeof(cl_int), &cl_size);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 7, sizeof(T), &alpha);
    SAMPLE_CHECK_ERRORS(err);
    err = clSetKernelArg(executable.kernel, 8, sizeof(T), &beta);
    SAMPLE_CHECK_ERRORS(err);

    // -----------------------------------------------------------------------
    // Define ndrange iteration space: global and local sizes based on
    // parameters obtained from user.

    // Refer to the sample documentation for clarification about
    // how work is devided among work-groups and work-items.
    // -----------------------------------------------------------------------

    size_t global_size[2] = {
        size / cmdparser.tile_size_M.getValue(),
        size / cmdparser.tile_size_N.getValue()
    };

    size_t local_size[2] = {
        cmdparser.tile_group_M.getValue(),
        cmdparser.tile_group_N.getValue()
    };

    // theoretical number of floating point operations (addition and multiplication) for one kernel execution
    // needed for performance calculations (GFLOPS) at every iteration below
    double flops = double(size)*size*(
        size + // multiplications
        size + // additions
        2      // multiplication by alpha and beta
    );

    // -----------------------------------------------------------------------
    // Loop with the kernel invocation
    // -----------------------------------------------------------------------

    for(int i = 0; i < cmdparser.iterations.getValue(); ++i)
    {
        // Here we start measuring host time for kernel execution
        double start = time_stamp();

        err = clEnqueueNDRangeKernel(
            oclobjects.queue,
            executable.kernel,
            2,
            0,
            global_size,
            local_size,
            0, 0, 0
        );
        SAMPLE_CHECK_ERRORS(err);

        err = clFinish(oclobjects.queue);
        SAMPLE_CHECK_ERRORS(err);

        // It is important to measure end host time after clFinish call
        double end = time_stamp();

        double time = end - start;
        cout << "Host time: " << time << " sec.\n";
        cout << "Host perf: " << flops/time/1e9 << " GFLOPS\n";

        if(i == 0 && cmdparser.validation.getValue())
        {
            // Validate result for the first iteration only and
            // only if user wants this.
            // Please note, validation procedure cannot be run at
            // futher iterations after the very first iteration,
            // as the results are being accumulated in C matrix
            // every iteration but validation procedures assumes that
            // C initial values are all zeros.

            clEnqueueMapBuffer(
                oclobjects.queue,
                matrix_C.device,
                CL_TRUE,    // blocking map
                CL_MAP_READ,
                0,
                matrix_memory_size,
                0, 0, 0,
                &err
            );
            SAMPLE_CHECK_ERRORS(err);

            // After map call, host-memory area for matrix C is
            // automatically updated with the latest bits from the device
            // So we just use it by original pointer as well as input matrices:
            if(
                !checkValidity(
                    matrix_A.host,
                    matrix_B.host,
                    matrix_C.host,
                    size,
                    stride,
                    cmdparser.kernel_nt.isSet(),    // whether B is transposed or not
                    alpha,
                    beta
                )
            )
            {
                throw Error("Validation procedure reported failures");
            }

            err = clEnqueueUnmapMemObject(
                oclobjects.queue,
                matrix_C.device,
                matrix_C.host,
                0, 0, 0
            );
            SAMPLE_CHECK_ERRORS(err);

            // Finish here is only required for correct time measurment on the next iteration
            // It does not affect correctness of calculations because you use the in-order OpenCL queue here.
            err = clFinish(oclobjects.queue);
            SAMPLE_CHECK_ERRORS(err);
        }
    }

    // All resources are deallocated automatically.
}


// Entry point for sample application, command-line parsing,
// generic OpenCL resources allocation and deallocation.
//int main (int argc, const char** argv)
//{
//    try
//    {
//        // Define and parse command-line arguments.
//        CmdParserGEMM cmdparser(argc, argv);
//        cmdparser.parse();
//
//        // Immediatly exit if user wanted to see the usage information only.
//        if(cmdparser.help.isSet())
//        {
//            return 0;
//        }
//
//        // Create the necessary OpenCL objects up to device queue.
//        OpenCLBasic oclobjects(
//            cmdparser.platform.getValue(),
//            cmdparser.device_type.getValue(),
//            cmdparser.device.getValue()
//        );
//
//        // Form build options string from given parameters: macros definitions to pass into kernels
//        string build_options =
//            "-DT=" + cmdparser.arithmetic.getValue() +
//            (cmdparser.arithmetic_double.isSet() ? " -DSAMPLE_NEEDS_DOUBLE" : "") +
//            " -DTILE_SIZE_M=" + to_str(cmdparser.tile_size_M.getValue()) +
//            " -DTILE_GROUP_M=" + to_str(cmdparser.tile_group_M.getValue()) +
//            " -DTILE_SIZE_N=" + to_str(cmdparser.tile_size_N.getValue()) +
//            " -DTILE_GROUP_N=" + to_str(cmdparser.tile_group_N.getValue()) +
//            " -DTILE_SIZE_K=" + to_str(cmdparser.tile_size_K.getValue());
//
//        cout << "Build program options: " << inquotes(build_options) << "\n";
//
//        // Build kernel
//        OpenCLProgramOneKernel executable(
//            oclobjects,
//            L"OpenCLFile1.cl",
//            "",
//            "gemm_" + cmdparser.kernel.getValue(),
//            build_options
//        );
//
//        // Call gemm with required type of elements
//        if(cmdparser.arithmetic_float.isSet())
//        {
//            gemm<float>(cmdparser, oclobjects, executable);
//        }
//        else if(cmdparser.arithmetic_double.isSet())
//        {
//            gemm<double>(cmdparser, oclobjects, executable);
//        }
//
//        // All resource deallocations happen in destructors of helper objects.
//
//        return 0;
//    }
//    catch(const CmdParser::Error& error)
//    {
//        cerr
//            << "[ ERROR ] In command line: " << error.what() << "\n"
//            << "Run " << argv[0] << " -h for usage info.\n";
//        return 1;
//    }
//    catch(const Error& error)
//    {
//        cerr << "[ ERROR ] Sample application specific error: " << error.what() << "\n";
//        return 1;
//    }
//    catch(const exception& error)
//    {
//        cerr << "[ ERROR ] " << error.what() << "\n";
//        return 1;
//    }
//    catch(...)
//    {
//        cerr << "[ ERROR ] Unknown/internal error happened.\n";
//        return 1;
//    }
//}
