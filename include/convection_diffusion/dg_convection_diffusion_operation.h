/*
 * DGConvDiffOperation.h
 *
 *  Created on: Aug 2, 2016
 *      Author: fehn
 */

#ifndef INCLUDE_CONVECTION_DIFFUSION_DG_CONVECTION_DIFFUSION_OPERATION_H_
#define INCLUDE_CONVECTION_DIFFUSION_DG_CONVECTION_DIFFUSION_OPERATION_H_

#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping_q.h>
#include <deal.II/lac/parallel_vector.h>
#include <deal.II/numerics/vector_tools.h>

#include "../convection_diffusion/boundary_descriptor.h"
#include "../convection_diffusion/convection_diffusion_operators.h"
#include "../convection_diffusion/field_functions.h"
#include "../convection_diffusion/input_parameters.h"
#include "../convection_diffusion/multigrid_preconditioner.h"
#include "operators/inverse_mass_matrix.h"
#include "operators/matrix_operator_base.h"
#include "solvers_and_preconditioners/inverse_mass_matrix_preconditioner.h"
#include "solvers_and_preconditioners/iterative_solvers.h"
#include "solvers_and_preconditioners/jacobi_preconditioner.h"


template<int dim, int fe_degree, typename value_type>
class DGConvDiffOperation : public MatrixOperatorBase
{
public:

  DGConvDiffOperation(parallel::distributed::Triangulation<dim> const &triangulation,
                      ConvDiff::InputParametersConvDiff const         &param_in)
    :
    fe(QGaussLobatto<1>(fe_degree+1)),
    mapping(fe_degree),
    dof_handler(triangulation),
    param(param_in)
  {}

  void setup(const std::vector<GridTools::PeriodicFacePair<typename Triangulation<dim>::cell_iterator> >
                                                                     periodic_face_pairs,
             std_cxx11::shared_ptr<BoundaryDescriptorConvDiff<dim> > boundary_descriptor_in,
             std_cxx11::shared_ptr<FieldFunctionsConvDiff<dim> >     field_functions_in)
  {
    ConditionalOStream pcout(std::cout, Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0);
    pcout << std::endl << "Setup convection-diffusion operation ..." << std::endl;

    this->periodic_face_pairs = periodic_face_pairs;
    boundary_descriptor = boundary_descriptor_in;
    field_functions = field_functions_in;

    create_dofs();

    initialize_matrix_free();

    setup_operators();

    pcout << std::endl << "... done!" << std::endl;
  }

  void setup_solver(double const scaling_factor_time_derivative_term_in=-1.0)
  {
    ConditionalOStream pcout(std::cout, Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0);
    pcout << std::endl << "Setup solver ..." << std::endl;

    // scaling factor of time derivative term has to be set before initializing preconditioners
    conv_diff_operator.set_scaling_factor_time_derivative_term(scaling_factor_time_derivative_term_in);

    // initialize preconditioner
    if(param.preconditioner == ConvDiff::Preconditioner::InverseMassMatrix)
    {
      preconditioner.reset(new InverseMassMatrixPreconditioner<dim, fe_degree, value_type, 1>(data,0,0));
    }
    else if(param.preconditioner == ConvDiff::Preconditioner::PointJacobi)
    {
      preconditioner.reset(new JacobiPreconditioner<value_type,
          ScalarConvDiffOperators::ConvectionDiffusionOperator<dim,fe_degree,value_type> >(conv_diff_operator));
    }
    else if(param.preconditioner == ConvDiff::Preconditioner::BlockJacobi)
    {
      preconditioner.reset(new BlockJacobiPreconditioner<value_type,
          ScalarConvDiffOperators::ConvectionDiffusionOperator<dim,fe_degree,value_type> >(conv_diff_operator));
    }
    else if(param.preconditioner == ConvDiff::Preconditioner::MultigridDiffusion)
    {
      MultigridData mg_data;
      mg_data = param.multigrid_data;

      typedef float Number;

      typedef MyMultigridPreconditionerScalarDiff<dim,value_type,
          ScalarConvDiffOperators::HelmholtzOperator<dim,fe_degree,Number>,
          ScalarConvDiffOperators::ConvectionDiffusionOperator<dim,fe_degree,value_type> > MULTIGRID;

      preconditioner.reset(new MULTIGRID());
      std_cxx11::shared_ptr<MULTIGRID> mg_preconditioner = std::dynamic_pointer_cast<MULTIGRID>(preconditioner);
      mg_preconditioner->initialize(mg_data,dof_handler,mapping,conv_diff_operator,this->periodic_face_pairs);
    }
    else if(param.preconditioner == ConvDiff::Preconditioner::MultigridConvectionDiffusion)
    {
       MultigridData mg_data;
       mg_data = param.multigrid_data;

       typedef float Number;

       typedef MyMultigridPreconditionerScalarConvDiff<dim,value_type,
           ScalarConvDiffOperators::ConvectionDiffusionOperator<dim,fe_degree,Number>,
           ScalarConvDiffOperators::ConvectionDiffusionOperator<dim,fe_degree,value_type> > MULTIGRID;

       preconditioner.reset(new MULTIGRID());
       std_cxx11::shared_ptr<MULTIGRID> mg_preconditioner = std::dynamic_pointer_cast<MULTIGRID>(preconditioner);
       mg_preconditioner->initialize(mg_data,dof_handler,mapping,conv_diff_operator,this->periodic_face_pairs);
    }
    else
    {
      AssertThrow(param.preconditioner == ConvDiff::Preconditioner::None ||
                  param.preconditioner == ConvDiff::Preconditioner::InverseMassMatrix ||
                  param.preconditioner == ConvDiff::Preconditioner::PointJacobi ||
                  param.preconditioner == ConvDiff::Preconditioner::BlockJacobi ||
                  param.preconditioner == ConvDiff::Preconditioner::MultigridDiffusion ||
                  param.preconditioner == ConvDiff::Preconditioner::MultigridConvectionDiffusion,
                  ExcMessage("Specified preconditioner is not implemented!"));
    }


    if(param.solver == ConvDiff::Solver::PCG)
    {
      // initialize solver_data
      CGSolverData solver_data;
      solver_data.solver_tolerance_abs = param.abs_tol;
      solver_data.solver_tolerance_rel = param.rel_tol;
      solver_data.max_iter = param.max_iter;
      solver_data.update_preconditioner = param.update_preconditioner;

      if(param.preconditioner != ConvDiff::Preconditioner::None)
        solver_data.use_preconditioner = true;

      // initialize solver
      iterative_solver.reset(new CGSolver<ScalarConvDiffOperators::ConvectionDiffusionOperator<dim,fe_degree,value_type>,
                                          PreconditionerBase<value_type>,
                                          parallel::distributed::Vector<value_type> >
                                 (conv_diff_operator,*preconditioner,solver_data));
    }
    else if(param.solver == ConvDiff::Solver::GMRES)
    {
      // initialize solver_data
      GMRESSolverData solver_data;
      solver_data.solver_tolerance_abs = param.abs_tol;
      solver_data.solver_tolerance_rel = param.rel_tol;
      solver_data.max_iter = param.max_iter;
      solver_data.right_preconditioning = param.use_right_preconditioner;
      solver_data.max_n_tmp_vectors = param.max_n_tmp_vectors;
      solver_data.update_preconditioner = param.update_preconditioner;

      if(param.preconditioner != ConvDiff::Preconditioner::None)
        solver_data.use_preconditioner = true;

      // initialize solver
      iterative_solver.reset(new GMRESSolver<ScalarConvDiffOperators::ConvectionDiffusionOperator<dim,fe_degree,value_type>,
                                             PreconditionerBase<value_type>,
                                             parallel::distributed::Vector<value_type> >
                                 (conv_diff_operator,*preconditioner,solver_data));
    }
    else
    {
      AssertThrow(param.solver == ConvDiff::Solver::PCG ||
                  param.solver == ConvDiff::Solver::GMRES,
                  ExcMessage("Specified solver is not implemented!"));
    }

    pcout << std::endl << "... done!" << std::endl;
  }

  void initialize_dof_vector(parallel::distributed::Vector<value_type> &src) const
  {
    data.initialize_dof_vector(src);
  }

  void prescribe_initial_conditions(parallel::distributed::Vector<value_type> &src,
                                    const double                              evaluation_time) const
  {
    field_functions->analytical_solution->set_time(evaluation_time);
    VectorTools::interpolate(dof_handler, *(field_functions->analytical_solution), src);
  }

  /*
   *  This function is used in case of explicit time integration:
   *  This function evaluates the right-hand side operator, the
   *  convective and diffusive term (subsequently multiplied by -1.0 in order
   *  to shift these terms to the right-hand side of the equations)
   *  and finally applies the inverse mass matrix operator.
   */
  void evaluate(parallel::distributed::Vector<value_type>       &dst,
                parallel::distributed::Vector<value_type> const &src,
                const value_type                                evaluation_time) const
  {
    if(param.runtime_optimization == false) //apply volume and surface integrals for each operator separately
    {
      // set dst to zero
      dst = 0.0;

      // diffusive operator
      if(param.equation_type == ConvDiff::EquationType::Diffusion ||
         param.equation_type == ConvDiff::EquationType::ConvectionDiffusion)
      {
        diffusive_operator.evaluate_add(dst,src,evaluation_time);
      }

      // convective operator
      if(param.equation_type == ConvDiff::EquationType::Convection ||
         param.equation_type == ConvDiff::EquationType::ConvectionDiffusion)
      {
        convective_operator.evaluate_add(dst,src,evaluation_time);
      }

      // shift diffusive and convective term to the rhs of the equation
      dst *= -1.0;

      if(param.right_hand_side == true)
      {
        rhs_operator.evaluate_add(dst,evaluation_time);
      }
    }
    else // param.runtime_optimization == true
    {
      convection_diffusion_operator_efficiency.evaluate(dst,src,evaluation_time);
    }

    // apply inverse mass matrix
    inverse_mass_matrix_operator.apply(dst,dst);
  }

  void evaluate_convective_term(parallel::distributed::Vector<value_type>       &dst,
                                parallel::distributed::Vector<value_type> const &src,
                                const value_type                                evaluation_time) const
  {
    convective_operator.evaluate(dst,src,evaluation_time);
  }

  /*
   *  This function calculates the inhomogeneous parts of all operators
   *  arising e.g. from inhomogeneous boundary conditions or the solution
   *  at previous instants of time occuring in the discrete time derivate
   *  term.
   *  Note that the convective operator only has a contribution if it is
   *  treated implicitly. In case of an explicit treatment the whole
   *  convective operator (function evaluate() instead of rhs()) has to be
   *  added to the right-hand side of the equations.
   */
  void rhs(parallel::distributed::Vector<value_type>       &dst,
           parallel::distributed::Vector<value_type> const *src = nullptr,
           double const                                    evaluation_time = 0.0) const
  {
    // mass matrix operator
    if(param.problem_type == ConvDiff::ProblemType::Steady)
    {
      dst = 0;
    }
    else if(param.problem_type == ConvDiff::ProblemType::Unsteady)
    {
      AssertThrow(src != nullptr, ExcMessage("src-Vector is invalid when evaluating rhs of scalar convection-diffusion equation."));

      mass_matrix_operator.apply(dst,*src);
    }
    else
    {
      AssertThrow(param.problem_type == ConvDiff::ProblemType::Steady ||
                  param.problem_type == ConvDiff::ProblemType::Unsteady,
                  ExcMessage("Specified problem type for convection-diffusion equation not implemented."));
    }

    // diffusive operator
    if(param.equation_type == ConvDiff::EquationType::Diffusion ||
       param.equation_type == ConvDiff::EquationType::ConvectionDiffusion)
    {
      diffusive_operator.rhs_add(dst,evaluation_time);
    }

    // convective operator
    if(param.equation_type == ConvDiff::EquationType::Convection ||
       param.equation_type == ConvDiff::EquationType::ConvectionDiffusion)
    {
      if(param.problem_type == ConvDiff::ProblemType::Steady
         ||
         (param.problem_type == ConvDiff::ProblemType::Unsteady &&
          param.treatment_of_convective_term == ConvDiff::TreatmentOfConvectiveTerm::Implicit))
      {
        convective_operator.rhs_add(dst,evaluation_time);
      }
    }

    if(param.right_hand_side == true)
    {
      rhs_operator.evaluate_add(dst,evaluation_time);
    }
  }

  unsigned int solve(parallel::distributed::Vector<value_type>       &sol,
                     parallel::distributed::Vector<value_type> const &rhs,
                     double const                                    scaling_factor_time_derivative_term_in = -1.0,
                     double const                                    evaluation_time_in = -1.0)
  {
    conv_diff_operator.set_scaling_factor_time_derivative_term(scaling_factor_time_derivative_term_in);
    conv_diff_operator.set_evaluation_time(evaluation_time_in);

    unsigned int iterations = iterative_solver->solve(sol,rhs);

    return iterations;
  }

  // getters
  MatrixFree<dim,value_type> const & get_data() const
  {
    return data;
  }

  Mapping<dim> const & get_mapping() const
  {
    return mapping;
  }

  DoFHandler<dim> const & get_dof_handler() const
  {
    return dof_handler;
  }


private:
  void create_dofs()
  {
    // enumerate degrees of freedom
    dof_handler.distribute_dofs(fe);
    dof_handler.distribute_mg_dofs(fe);

    unsigned int ndofs_per_cell = Utilities::fixed_int_power<fe_degree+1,dim>::value;

    ConditionalOStream pcout(std::cout, Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0);

    pcout << std::endl
          << "Discontinuous Galerkin finite element discretization:" << std::endl << std::endl;

    print_parameter(pcout,"degree of 1D polynomials",fe_degree);
    print_parameter(pcout,"number of dofs per cell",ndofs_per_cell);
    print_parameter(pcout,"number of dofs (total)",dof_handler.n_dofs());
  }

  void initialize_matrix_free()
  {
    // quadrature formula used to perform integrals
    QGauss<1> quadrature (fe_degree+1);

    // initialize matrix_free_data
    typename MatrixFree<dim,value_type>::AdditionalData additional_data;
    additional_data.tasks_parallel_scheme =
      MatrixFree<dim,value_type>::AdditionalData::partition_partition;
    additional_data.build_face_info = true;
    additional_data.mapping_update_flags = (update_gradients | update_JxW_values |
                        update_quadrature_points | update_normal_vectors |
                        update_values);

    ConstraintMatrix dummy;
    dummy.close();
    data.reinit (mapping, dof_handler, dummy, quadrature, additional_data);
  }

  void setup_operators()
  {
    // mass matrix operator
    mass_matrix_operator_data.dof_index = 0;
    mass_matrix_operator_data.quad_index = 0;
    mass_matrix_operator.initialize(data,mass_matrix_operator_data);

    // inverse mass matrix operator
    // dof_index = 0, quad_index = 0
    inverse_mass_matrix_operator.initialize(data,0,0);

    // convective operator
    convective_operator_data.dof_index = 0;
    convective_operator_data.quad_index = 0;
    convective_operator_data.numerical_flux_formulation = param.numerical_flux_convective_operator;
    convective_operator_data.bc = boundary_descriptor;
    convective_operator_data.velocity = field_functions->velocity;
    convective_operator.initialize(data,convective_operator_data);

    // diffusive operator
    diffusive_operator_data.dof_index = 0;
    diffusive_operator_data.quad_index = 0;
    diffusive_operator_data.IP_factor = param.IP_factor;
    diffusive_operator_data.diffusivity = param.diffusivity;
    diffusive_operator_data.bc = boundary_descriptor;
    diffusive_operator.initialize(mapping,data,diffusive_operator_data);

    // rhs operator
    ScalarConvDiffOperators::RHSOperatorData<dim> rhs_operator_data;
    rhs_operator_data.dof_index = 0;
    rhs_operator_data.quad_index = 0;
    rhs_operator_data.rhs = field_functions->right_hand_side;
    rhs_operator.initialize(data,rhs_operator_data);

    // convection-diffusion operator
    ScalarConvDiffOperators::ConvectionDiffusionOperatorData<dim> conv_diff_operator_data;
    if(this->param.problem_type == ConvDiff::ProblemType::Unsteady)
    {
      conv_diff_operator_data.unsteady_problem = true;
    }
    else
    {
      conv_diff_operator_data.unsteady_problem = false;
    }

    if(this->param.equation_type == ConvDiff::EquationType::Diffusion ||
       this->param.equation_type == ConvDiff::EquationType::ConvectionDiffusion)
    {
      conv_diff_operator_data.diffusive_problem = true;
    }
    else
    {
      conv_diff_operator_data.diffusive_problem = false;
    }

    if((this->param.equation_type == ConvDiff::EquationType::Convection ||
        this->param.equation_type == ConvDiff::EquationType::ConvectionDiffusion)
        &&
       this->param.treatment_of_convective_term == ConvDiff::TreatmentOfConvectiveTerm::Implicit)
    {
      conv_diff_operator_data.convective_problem = true;
    }
    else
    {
      conv_diff_operator_data.convective_problem = false;
    }

    conv_diff_operator_data.dof_index = 0;

    conv_diff_operator.initialize(data,
                                  conv_diff_operator_data,
                                  mass_matrix_operator,
                                  convective_operator,
                                  diffusive_operator);



    // convection-diffusion operator (efficient implementation, only for explicit time integration, includes also rhs operator)
    ScalarConvDiffOperators::ConvectionDiffusionOperatorDataEfficiency<dim> conv_diff_operator_data_eff;
    conv_diff_operator_data_eff.conv_data = convective_operator_data;
    conv_diff_operator_data_eff.diff_data = diffusive_operator_data;
    conv_diff_operator_data_eff.rhs_data = rhs_operator_data;
    convection_diffusion_operator_efficiency.initialize(mapping, data, conv_diff_operator_data_eff);
  }


  FE_DGQArbitraryNodes<dim> fe;
  MappingQGeneric<dim> mapping;
  DoFHandler<dim> dof_handler;

  MatrixFree<dim,value_type> data;

  ConvDiff::InputParametersConvDiff const &param;

  // TODO This variable is only needed when using the GeometricMultigrid preconditioner
  std::vector<GridTools::PeriodicFacePair<typename Triangulation<dim>::cell_iterator> > periodic_face_pairs;

  std_cxx11::shared_ptr<BoundaryDescriptorConvDiff<dim> > boundary_descriptor;
  std_cxx11::shared_ptr<FieldFunctionsConvDiff<dim> > field_functions;

  ScalarConvDiffOperators::MassMatrixOperatorData mass_matrix_operator_data;
  ScalarConvDiffOperators::MassMatrixOperator<dim, fe_degree, value_type> mass_matrix_operator;
  InverseMassMatrixOperator<dim,fe_degree,value_type> inverse_mass_matrix_operator;

  ScalarConvDiffOperators::ConvectiveOperatorData<dim> convective_operator_data;
  ScalarConvDiffOperators::ConvectiveOperator<dim, fe_degree, value_type> convective_operator;

  ScalarConvDiffOperators::DiffusiveOperatorData<dim> diffusive_operator_data;
  ScalarConvDiffOperators::DiffusiveOperator<dim, fe_degree, value_type> diffusive_operator;
  ScalarConvDiffOperators::RHSOperator<dim, fe_degree, value_type> rhs_operator;

  ScalarConvDiffOperators::ConvectionDiffusionOperator<dim, fe_degree, value_type> conv_diff_operator;

  // convection-diffusion operator for runtime optimization (also includes rhs operator)
  ScalarConvDiffOperators::ConvectionDiffusionOperatorEfficiency<dim, fe_degree, value_type> convection_diffusion_operator_efficiency;

  std_cxx11::shared_ptr<PreconditionerBase<value_type> > preconditioner;
  std_cxx11::shared_ptr<IterativeSolverBase<parallel::distributed::Vector<value_type> > > iterative_solver;
};


#endif /* INCLUDE_CONVECTION_DIFFUSION_DG_CONVECTION_DIFFUSION_OPERATION_H_ */