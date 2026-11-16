#include "cutfem/experiments.hpp"

#include <iostream>
#include <stdexcept>
#ifdef CUTFEM_WITH_HYPRE
#include <HYPRE.h>
#include <HYPRE_utilities.h>
#include <mpi.h>
#endif
int main(int argc, char** argv) {
#ifdef CUTFEM_WITH_HYPRE
    MPI_Init(&argc, &argv);
    HYPRE_Init();
    int size = 0;
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size != 1) {
        std::cerr << "Only one MPI rank is supported\n";
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
#endif
    int result = 0;
    try {
        using namespace chipcae::proto_cutfem;
        if (argc < 2)
            throw std::runtime_error(
                "Usage: cutfem_proto "
                "{unit_tests|mms_thermal|mms_thermal_bimat|mms_elastic|timoshenko|gp_sweep} "
                "[--output DIR] [--gamma-N X] [--gamma-g X] [--N N] [--solver direct|pcg|hypre] "
                "[--quadrature halfplane|recursive] [--max-depth D] [--dump-matrices]");
        Options o;
        std::string experiment = argv[1];
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--dump-matrices") {
                o.dump_matrices = true;
                continue;
            }
            if (i + 1 >= argc)
                throw std::runtime_error("Missing argument for " + a);
            std::string b = argv[++i];
            if (a == "--output")
                o.output = b;
            else if (a == "--gamma-N" || a == "--gamma_N")
                o.gamma_N = std::stod(b);
            else if (a == "--gamma-g" || a == "--gamma_g") {
                o.gamma_g = std::stod(b);
                o.gp_specified = true;
            } else if (a == "--N")
                o.N = std::stoi(b);
            else if (a == "--solver")
                o.solver = b;
            else if (a == "--quadrature") {
                if (b != "halfplane" && b != "recursive")
                    throw std::runtime_error("Unknown quadrature");
                o.quadrature.recursive = b == "recursive";
            } else if (a == "--max-depth")
                o.quadrature.max_depth = std::stoi(b);
            else
                throw std::runtime_error("Unknown option " + a);
        }
        if (o.gamma_N <= 0 || o.gamma_g < 0 || o.N < 0)
            throw std::runtime_error("Invalid parameter");
        if (o.solver != "direct" && o.solver != "pcg" && o.solver != "hypre")
            throw std::runtime_error("Unknown solver");
        if (o.solver == "hypre" && !hypre_available())
            throw std::runtime_error("Rebuild with CUTFEM_WITH_HYPRE=ON");
        if (experiment == "unit_tests")
            result = run_unit_tests();
        else if (experiment == "mms_thermal")
            run_mms(false, false, o);
        else if (experiment == "mms_thermal_bimat")
            run_mms(false, true, o);
        else if (experiment == "mms_elastic")
            run_mms(true, false, o);
        else if (experiment == "timoshenko")
            run_timoshenko(o);
        else if (experiment == "gp_sweep")
            run_gp_sweep(o);
        else
            throw std::runtime_error("Unknown experiment " + experiment);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        result = 1;
    }
#ifdef CUTFEM_WITH_HYPRE
    HYPRE_Finalize();
    MPI_Finalize();
#endif
    return result;
}
