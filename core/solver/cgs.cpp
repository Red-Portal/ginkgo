/*******************************<GINKGO LICENSE>******************************
Copyright 2017-2018

Karlsruhe Institute of Technology
Universitat Jaume I
University of Tennessee

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************<GINKGO LICENSE>*******************************/

#include "core/solver/cgs.hpp"


#include "core/base/exception.hpp"
#include "core/base/exception_helpers.hpp"
#include "core/base/executor.hpp"
#include "core/base/math.hpp"
#include "core/base/utils.hpp"
#include "core/solver/cgs_kernels.hpp"


namespace gko {
namespace solver {


namespace {


template <typename ValueType>
struct TemplatedOperation {
    GKO_REGISTER_OPERATION(initialize, cgs::initialize<ValueType>);
    GKO_REGISTER_OPERATION(test_convergence, cgs::test_convergence<ValueType>);
    GKO_REGISTER_OPERATION(step_1, cgs::step_1<ValueType>);
    GKO_REGISTER_OPERATION(step_2, cgs::step_2<ValueType>);
    GKO_REGISTER_OPERATION(step_3, cgs::step_3<ValueType>);
};


}  // namespace


template <typename ValueType>
void Cgs<ValueType>::apply_impl(const LinOp *b, LinOp *x) const
{
    using std::swap;
    using Vector = matrix::Dense<ValueType>;
    auto dense_b = as<const Vector>(b);
    auto dense_x = as<Vector>(x);

    ASSERT_CONFORMANT(system_matrix_, b);
    ASSERT_EQUAL_DIMENSIONS(b, x);

    auto exec = this->get_executor();
    size_type num_vectors = dense_b->get_dimensions().num_cols;

    auto one_op = initialize<Vector>({one<ValueType>()}, exec);
    auto neg_one_op = initialize<Vector>({-one<ValueType>()}, exec);

    auto r = Vector::create_with_config_of(dense_b);
    auto r_tld = Vector::create_with_config_of(dense_b);
    auto p = Vector::create_with_config_of(dense_b);
    auto q = Vector::create_with_config_of(dense_b);
    auto u = Vector::create_with_config_of(dense_b);
    auto u_hat = Vector::create_with_config_of(dense_b);
    auto v_hat = Vector::create_with_config_of(dense_b);
    auto t = Vector::create_with_config_of(dense_b);

    auto alpha = Vector::create(exec, 1, dense_b->get_dimensions().num_cols);
    auto beta = Vector::create_with_config_of(alpha.get());
    auto gamma = Vector::create_with_config_of(alpha.get());
    auto rho_prev = Vector::create_with_config_of(alpha.get());
    auto rho = Vector::create_with_config_of(alpha.get());
    auto tau = Vector::create_with_config_of(alpha.get());

    auto starting_tau = Vector::create_with_config_of(tau.get());

    Array<bool> converged(exec, dense_b->get_dimensions().num_cols);

    // TODO: replace this with automatic merged kernel generator
    exec->run(TemplatedOperation<ValueType>::make_initialize_operation(
        dense_b, r.get(), r_tld.get(), p.get(), q.get(), u.get(), u_hat.get(),
        v_hat.get(), t.get(), alpha.get(), beta.get(), gamma.get(),
        rho_prev.get(), rho.get(), &converged));
    // r = dense_b
    // r_tld = r
    // rho = 0.0
    // rho_prev = 1.0
    // p = q = u = u_hat = v_hat = t = 0

    system_matrix_->apply(neg_one_op.get(), dense_x, one_op.get(), r.get());
    r->compute_dot(r.get(), tau.get());
    starting_tau->copy_from(tau.get());

    r_tld->copy_from(r.get());
    for (int iter = 0; iter < parameters_.max_iters; iter += 2) {
        r->compute_dot(r_tld.get(), rho.get());
        exec->run(TemplatedOperation<ValueType>::make_step_1_operation(
            r.get(), u.get(), p.get(), q.get(), beta.get(), rho.get(),
            rho_prev.get(), converged));
        // beta = rho / rho_prev
        // u = r + beta * q;
        // p = u + beta * ( q + beta * p );
        preconditioner_->apply(p.get(), t.get());
        system_matrix_->apply(t.get(), v_hat.get());
        r_tld->compute_dot(v_hat.get(), gamma.get());
        exec->run(TemplatedOperation<ValueType>::make_step_2_operation(
            u.get(), v_hat.get(), q.get(), t.get(), alpha.get(), rho.get(),
            gamma.get(), converged));
        // alpha = rho / gamma
        // q = u - alpha * v_hat
        // t = u + q
        preconditioner_->apply(t.get(), u_hat.get());
        system_matrix_->apply(u_hat.get(), t.get());
        exec->run(TemplatedOperation<ValueType>::make_step_3_operation(
            t.get(), u_hat.get(), r.get(), dense_x, alpha.get(), converged));
        // r = r -alpha * t
        // x = x + alpha * u_hat

        r->compute_dot(r.get(), tau.get());
        bool all_converged = false;
        exec->run(
            TemplatedOperation<ValueType>::make_test_convergence_operation(
                tau.get(), starting_tau.get(), parameters_.rel_residual_goal,
                &converged, &all_converged));
        if (all_converged) {
            break;
        }

        swap(rho_prev, rho);
    }
}


template <typename ValueType>
void Cgs<ValueType>::apply_impl(const LinOp *alpha, const LinOp *b,
                                const LinOp *beta, LinOp *x) const
{
    auto dense_x = as<matrix::Dense<ValueType>>(x);

    auto x_clone = dense_x->clone();
    this->apply(b, x_clone.get());
    dense_x->scale(beta);
    dense_x->add_scaled(alpha, x_clone.get());
}


#define GKO_DECLARE_CGS(_type) class Cgs<_type>
GKO_INSTANTIATE_FOR_EACH_VALUE_TYPE(GKO_DECLARE_CGS);
#undef GKO_DECLARE_CGS


}  // namespace solver
}  // namespace gko
