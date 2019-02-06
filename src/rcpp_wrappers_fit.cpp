#include <string.h>
#include "DataFunctions.h"
#include "Hierr.h"
#include "HierrUtils.h"
#include "CoordDescTypes.h"
#include "BinomialSolver.h"

template <typename TX, typename TZ>
Rcpp::List fitModel(const TX & x,
                    const bool & is_sparse_x,
                    const Eigen::Ref<const Eigen::VectorXd> & y,
                    const TZ & ext,
                    const Eigen::Ref<const Eigen::MatrixXd> & fixed,
                    Eigen::VectorXd weights_user,
                    const Rcpp::LogicalVector & intr,
                    const Rcpp::LogicalVector & stnd,
                    const Eigen::Ref<const Eigen::VectorXd> & penalty_type,
                    const Eigen::Ref<const Eigen::VectorXd> & cmult,
                    const Eigen::Ref<const Eigen::VectorXd> & quantiles,
                    const Rcpp::IntegerVector & num_penalty,
                    const Rcpp::NumericVector & penalty_ratio,
                    const Eigen::Ref<const Eigen::VectorXd> & penalty_user,
                    const Eigen::Ref<const Eigen::VectorXd> & penalty_user_ext,
                    Eigen::VectorXd lower_cl,
                    Eigen::VectorXd upper_cl,
                    const std::string & family,
                    const double & thresh,
                    const int & maxit,
                    const int & ne,
                    const int & nx) {

    // initialize objects to hold means, variances, sds of all variables
    const int n = x.rows();
    const int nv_x = x.cols();
    const int nv_fixed = fixed.size() == 0 ? 0 : fixed.cols();
    const int nv_ext = ext.size() == 0 ? 0 : ext.cols();
    const int nv_total = nv_x + nv_fixed + intr[1] + nv_ext;
    Eigen::VectorXd xm = Eigen::VectorXd::Constant(nv_total, 0.0);
    Eigen::VectorXd cent = Eigen::VectorXd::Constant(nv_total, 0.0);
    Eigen::VectorXd xv = Eigen::VectorXd::Constant(nv_total, 1.0);
    Eigen::VectorXd xs = Eigen::VectorXd::Constant(nv_total, 1.0);

    // map to correct size of fixed matrix
    Eigen::Map<const Eigen::MatrixXd> fixedmap(fixed.data(), fixed.rows(), nv_fixed);

    // scale user weights
    weights_user.array() = weights_user.array() / weights_user.sum();

    // compute moments of matrices and create XZ (if external data present)
    const bool center_x = intr[0] && !is_sparse_x;
    compute_moments(x, weights_user, xm, cent, xv, xs, center_x, stnd[0], 0);
    compute_moments(fixedmap, weights_user, xm, cent, xv, xs, center_x, stnd[0], nv_x);
    const Eigen::MatrixXd xz = create_XZ(
        x, ext, xm, cent, weights_user, xv,
        xs, intr[1], stnd[1], nv_x + nv_fixed
    );

    // standardize y if continuous
    Eigen::VectorXd yscaled(y.size());
    double ys = 1.0;
    double ym = 0.0;
    if (family == "gaussian") {
        ym = y.cwiseProduct(weights_user).sum();
        ys = std::sqrt(y.cwiseProduct(y.cwiseProduct(weights_user)).sum() - ym * ym);
        if (!intr[0])
            ym = 0.0;
        yscaled.array() = y.array() / ys;
    }

    // choose solver based on outcome
    std::unique_ptr<CoordSolver<TX> > solver;
    if (family == "gaussian") {
        solver.reset(
            new CoordSolver<TX>(
                yscaled, x, fixedmap, xz, cent.data(), xv.data(), xs.data(),
                weights_user, intr[0], penalty_type.data(),
                cmult.data(), quantiles, upper_cl.data(),
                lower_cl.data(), ne, nx, thresh, maxit
            )
        );

    }
    else if (family == "binomial") {
        solver.reset(
            new BinomialSolver<TX>(
                yscaled, x, fixedmap, xz, cent.data(), xv.data(), xs.data(),
                weights_user, intr[0], penalty_type.data(),
                cmult.data(), quantiles, upper_cl.data(),
                lower_cl.data(), ne, nx, thresh, maxit
            )
        );
    }

    // Object to hold results for all penalty combinations
    const int num_combn = num_penalty[0] * num_penalty[1];
    Hierr<TX, TZ> estimates = Hierr<TX, TZ>(
        n, nv_x, nv_fixed, nv_ext, nv_total,
        intr[0], intr[1], ext, xm.data(), cent.data(),
        xs.data(), ym, ys, num_combn
    );

    // compute penalty path for 1st level variables
    Eigen::VectorXd path(num_penalty[0]);

    compute_penalty(
        path, penalty_user, penalty_type[0],
        penalty_ratio[0], solver->getGradient(),
        solver->getCmult(), 0, nv_x, ys
    );

    // compute penalty path for 2nd level variables
    Eigen::VectorXd path_ext(num_penalty[1]);
    if (nv_ext > 0) {
        compute_penalty(
            path_ext, penalty_user_ext,
            penalty_type[nv_x + nv_fixed + intr[1]],
            penalty_ratio[1], solver->getGradient(),
            solver->getCmult(), nv_x + nv_fixed + intr[1],
            nv_total, ys
        );
    } else {
        path_ext[0] = 0.0;
    }

    // solve grid of penalties in decreasing order
    double b0_outer = solver->getBeta0();
    Eigen::VectorXd betas_outer = solver->getBetas();
    Eigen::VectorXd strong_sum = Eigen::VectorXd::Zero(num_combn);
    Eigen::VectorXd active_sum = Eigen::VectorXd::Zero(num_combn);

    int idx_pen = 0;
    for (int m = 0; m < num_penalty[0]; ++m) {
        solver->setPenalty(path[m], 0);
        for (int m2 = 0; m2 < num_penalty[1]; ++m2, ++idx_pen) {
            solver->setPenalty(path_ext[m2], 1);
            if (m2 == 0 && num_penalty[1] > 1) {
                solver->warm_start(y, b0_outer, betas_outer);
                solver->update_strong(path, path_ext, m, m2);
                solver->solve();
                b0_outer = solver->getBeta0();
                betas_outer = solver->getBetas();
            }
            else {
                solver->update_strong(path, path_ext, m, m2);
                solver->solve();
            }
            strong_sum[idx_pen] = sum(solver->getStrongSet());
            active_sum[idx_pen] = sum(solver->getActiveSet());
            estimates.add_results(solver->getBeta0(), solver->getBetas(), idx_pen);
        }
    }

    // fix first penalties (when path automatically computed)
    if (penalty_user[0] == 0.0) {
        path[0] = exp(2 * log(path[1]) - log(path[2]));
    }
    if (penalty_user_ext[0] == 0.0 && nv_ext > 0) {
        path_ext[0] = exp(2 * log(path_ext[1]) - log(path_ext[2]));
    }

    int status = 0;

    // collect results in list and return to R
    return Rcpp::List::create(
            Rcpp::Named("beta0") = estimates.getBeta0(),
            Rcpp::Named("betas") = estimates.getBetas(),
            Rcpp::Named("alpha0") = estimates.getAlpha0(),
            Rcpp::Named("alphas") = estimates.getAlphas(),
            Rcpp::Named("penalty") = ys * path,
            Rcpp::Named("penalty_ext") = ys * path_ext,
            Rcpp::Named("num_passes") = solver->getNumPasses(),
            Rcpp::Named("family") = family,
            Rcpp::Named("status") = status
        );
}


// [[Rcpp::export]]
Rcpp::List fitModelRcpp(SEXP x,
                        const int & mattype_x,
                        const Eigen::Map<Eigen::VectorXd> y,
                        SEXP ext,
                        const bool & is_sparse_ext,
                        const Eigen::Map<Eigen::MatrixXd> fixed,
                        Eigen::VectorXd weights_user,
                        const Rcpp::LogicalVector & intr,
                        const Rcpp::LogicalVector & stnd,
                        const Eigen::Map<Eigen::VectorXd> penalty_type,
                        const Eigen::Map<Eigen::VectorXd> cmult,
                        const Eigen::Map<Eigen::VectorXd> quantiles,
                        const Rcpp::IntegerVector & num_penalty,
                        const Rcpp::NumericVector & penalty_ratio,
                        const Eigen::Map<Eigen::VectorXd> penalty_user,
                        const Eigen::Map<Eigen::VectorXd> penalty_user_ext,
                        Eigen::VectorXd lower_cl,
                        Eigen::VectorXd upper_cl,
                        const std::string & family,
                        const double & thresh,
                        const int & maxit,
                        const int & ne,
                        const int & nx) {

    if (mattype_x == 1) {
        const bool is_sparse_x = false;
        Rcpp::NumericMatrix x_mat(x);
        MapMat xmap((const double *) &x_mat[0], x_mat.rows(), x_mat.cols());
        if (is_sparse_ext)
            return fitModel<MapMat, MapSpMat>(
                    xmap, is_sparse_x, y, Rcpp::as<MapSpMat>(ext),
                    fixed, weights_user, intr, stnd, penalty_type, cmult,
                    quantiles, num_penalty, penalty_ratio, penalty_user,
                    penalty_user_ext, lower_cl, upper_cl, family, thresh,
                    maxit, ne, nx
                );
        else {
            Rcpp::NumericMatrix ext_mat(ext);
            MapMat extmap((const double *) &ext_mat[0], ext_mat.rows(), ext_mat.cols());
            return fitModel<MapMat, MapMat>(
                    xmap, is_sparse_x, y, extmap, fixed, weights_user,
                    intr, stnd, penalty_type, cmult, quantiles, num_penalty,
                    penalty_ratio, penalty_user, penalty_user_ext, lower_cl,
                    upper_cl, family, thresh, maxit, ne, nx
                );
        }
    } else if (mattype_x == 2) {
        const bool is_sparse_x = false;
        Rcpp::XPtr<BigMatrix> xptr(x);
        MapMat xmap((const double *)xptr->matrix(), xptr->nrow(), xptr->ncol());
        if (is_sparse_ext) {
            return fitModel<MapMat, MapSpMat>(
                    xmap, is_sparse_x, y, Rcpp::as<MapSpMat>(ext), fixed, weights_user,
                    intr, stnd, penalty_type, cmult, quantiles, num_penalty,
                    penalty_ratio, penalty_user, penalty_user_ext, lower_cl,
                    upper_cl, family, thresh, maxit, ne, nx
            );
        }
        else {
            Rcpp::NumericMatrix ext_mat(ext);
            MapMat extmap((const double *) &ext_mat[0], ext_mat.rows(), ext_mat.cols());
            return fitModel<MapMat, MapMat>(
                    xmap, is_sparse_x, y, extmap, fixed, weights_user, intr, stnd,
                    penalty_type, cmult, quantiles, num_penalty,
                    penalty_ratio, penalty_user, penalty_user_ext, lower_cl,
                    upper_cl, family, thresh, maxit, ne, nx
            );
        }
    } else {
        const bool is_sparse_x = true;
        if (is_sparse_ext)
            return fitModel<MapSpMat, MapSpMat>(
                    Rcpp::as<MapSpMat>(x), is_sparse_x, y, Rcpp::as<MapSpMat>(ext),
                    fixed, weights_user, intr, stnd, penalty_type, cmult,
                    quantiles, num_penalty, penalty_ratio, penalty_user,
                    penalty_user_ext, lower_cl, upper_cl, family, thresh,
                    maxit, ne, nx
            );
        else {
            Rcpp::NumericMatrix ext_mat(ext);
            MapMat extmap((const double *) &ext_mat[0], ext_mat.rows(), ext_mat.cols());
            return fitModel<MapSpMat, MapMat>(
                    Rcpp::as<MapSpMat>(x), is_sparse_x, y, extmap, fixed, weights_user,
                    intr, stnd, penalty_type, cmult, quantiles, num_penalty,
                    penalty_ratio, penalty_user, penalty_user_ext, lower_cl,
                    upper_cl, family, thresh, maxit, ne, nx
            );
        }
    }
}


/*
 * Rcpp wrapper to fit model when x is sparse matrix
 *  and external is dense matrix


// [[Rcpp::export]]
Rcpp::List fitSparseDense(const Eigen::MappedSparseMatrix<double> x,
                          const Eigen::Map<Eigen::VectorXd> y,
                          const Eigen::Map<Eigen::MatrixXd> ext,
                          const Eigen::Map<Eigen::MatrixXd> fixed,
                          Eigen::VectorXd weights_user,
                          const Rcpp::LogicalVector & intr,
                          const Rcpp::LogicalVector & stnd,
                          const Eigen::Map<Eigen::VectorXd> penalty_type,
                          const Eigen::Map<Eigen::VectorXd> cmult,
                          const Eigen::Map<Eigen::VectorXd> quantiles,
                          const Rcpp::IntegerVector & num_penalty,
                          const Rcpp::NumericVector & penalty_ratio,
                          const Eigen::Map<Eigen::VectorXd> penalty_user,
                          const Eigen::Map<Eigen::VectorXd> penalty_user_ext,
                          Eigen::VectorXd lower_cl,
                          Eigen::VectorXd upper_cl,
                          const std::string & family,
                          const double & thresh,
                          const int & maxit,
                          const int & ne,
                          const int & nx) {

    // initialize objects to hold means, variances, sds of all variables
    const int n = x.rows();
    const int nv_x = x.cols();
    const int nv_fixed = fixed.size() == 0 ? 0 : fixed.cols();
    const int nv_ext = ext.size() == 0 ? 0 : ext.cols();
    const int nv_total = nv_x + nv_fixed + intr[1] + nv_ext;
    Eigen::VectorXd xm = Eigen::VectorXd::Constant(nv_total, 0.0);
    Eigen::VectorXd xv = Eigen::VectorXd::Constant(nv_total, 1.0);
    Eigen::VectorXd xs = Eigen::VectorXd::Constant(nv_total, 1.0);

    // map to correct size of fixed matrix
    Eigen::Map<const Eigen::MatrixXd> fixedmap(fixed.data(), fixed.rows(), nv_fixed);

    // scale user weights
    weights_user.array() = weights_user.array() / weights_user.sum();

    // compute moments of matrices and create XZ (if external data present)
    compute_moments(x, weights_user, xm, xv, xs, true, stnd[0], 0);
    compute_moments(fixedmap, weights_user, xm, xv, xs, true, stnd[0], nv_x);
    const Eigen::MatrixXd xz = create_XZ(x, ext, xm, xv, xs, intr[1]);
    compute_moments(xz, weights_user, xm, xv, xs, false, stnd[1], nv_x + nv_fixed);

    // choose solver based on family
    std::unique_ptr<CoordSolver<MapSpMat> > solver;
    if (family == "gaussian") {
        solver.reset(new CoordSolver<MapSpMat>(y, x, fixed, xz, xm.data(), xv.data(), xs.data(),
                                               weights_user, intr, penalty_type.data(),
                                               cmult.data(), quantiles,
                                               upper_cl.data(), lower_cl.data(),
                                               ne, nx, thresh, maxit));
    }
    else if (family == "binomial") {
        solver.reset(new BinomialSolver<MapSpMat>(y, x, fixed, xz, xm.data(), xv.data(), xs.data(),
                                                  weights_user, intr, penalty_type.data(),
                                                  cmult.data(), quantiles,
                                                  upper_cl.data(), lower_cl.data(),
                                                  ne, nx, thresh, maxit));

    }

    // Object to hold results for all penalty combinations
    const int num_combn = num_penalty[0] * num_penalty[1];
    Hierr<MapSpMat, MapMat> estimates = Hierr<MapSpMat, MapMat>(n, nv_x, nv_fixed, nv_ext, nv_total,
                                                                intr[0], intr[1], ext.data(), xm.data(),
                                                                xs.data(), num_combn);

    // compute penalty path for 1st level variables
    Eigen::VectorXd path(num_penalty[0]);

    compute_penalty(path, penalty_user, penalty_type[0],
                    penalty_ratio[0], solver->getGradient(),
                    solver->getCmult(), 0, nv_x);

    // compute penalty path for 2nd level variables
    Eigen::VectorXd path_ext(num_penalty[1]);
    if (nv_ext > 0) {
        compute_penalty(path_ext, penalty_user_ext, penalty_type[nv_x + nv_fixed + intr[1]],
                        penalty_ratio[1], solver->getGradient(),
                        solver->getCmult(), nv_x + nv_fixed + intr[1], nv_total);
    } else {
        path_ext[0] = 0.0;
    }

    // solve grid of penalties in decreasing order
    double b0_outer = solver->getBeta0();
    Eigen::VectorXd betas_outer = solver->getBetas();
    Eigen::VectorXd strong_sum = Eigen::VectorXd::Zero(num_combn);
    Eigen::VectorXd active_sum = Eigen::VectorXd::Zero(num_combn);

    int idx_pen = 0;
    for (int m = 0; m < num_penalty[0]; ++m) {
        solver->setPenalty(path[m], 0);
        for (int m2 = 0; m2 < num_penalty[1]; ++m2, ++idx_pen) {
            solver->setPenalty(path_ext[m2], 1);
            if (m2 == 0 && num_penalty[1] > 1) {
                solver->warm_start(y, b0_outer, betas_outer);
                solver->update_strong(path, path_ext, m, m2);
                solver->solve();
                b0_outer = solver->getBeta0();
                betas_outer = solver->getBetas();
            }
            else {
                solver->update_strong(path, path_ext, m, m2);
                solver->solve();
            }
            strong_sum[idx_pen] = sum(solver->getStrongSet());
            active_sum[idx_pen] = sum(solver->getActiveSet());
            estimates.add_results(solver->getBeta0(), solver->getBetas(), idx_pen);
        }
    }

    int status = 0;

    // collect results in list and return to R
    return Rcpp::List::create(Rcpp::Named("beta0") = estimates.getBeta0(),
                              Rcpp::Named("betas") = estimates.getBetas(),
                              Rcpp::Named("alpha0") = estimates.getAlpha0(),
                              Rcpp::Named("alphas") = estimates.getAlphas(),
                              Rcpp::Named("num_passes") = solver->getNumPasses(),
                              Rcpp::Named("penalty") = path,
                              Rcpp::Named("penalty_ext") = path_ext,
                              Rcpp::Named("strong_sum") = strong_sum,
                              Rcpp::Named("active_sum") = active_sum,
                              Rcpp::Named("status") = status);
}

 */
