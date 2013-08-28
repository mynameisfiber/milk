#include <algorithm>

extern "C" {
    #include <Python.h>
    #include <numpy/ndarrayobject.h>
}

namespace {
struct Kmeans_Exception {
    Kmeans_Exception(const char* msg): msg(msg) { }
    const char* msg;

};
void assert_type_contiguous(PyArrayObject* array,int type) { 
    if (!PyArray_Check(array) ||
        PyArray_TYPE(array) != type ||
        !PyArray_ISCONTIGUOUS(array)) {
        throw Kmeans_Exception("Arguments to kmeans don't conform to expectation. Are you calling this directly? This is an internal function!");
    }
}

template <typename ftype>
bool assignclass_euclidian(ftype* f, ftype* centroids, PyArrayObject* a_assignments, const int N, const int Nf, const int k) {
    Py_BEGIN_ALLOW_THREADS
    npy_int32* assignments = static_cast<npy_int32*>(PyArray_DATA(a_assignments));

    #pragma parallel for schedule(static)
    for (int i=0; i < N; i++) {
        int best_cluster = -1;
        ftype min_distance = 0.0;
        for (int c=0; c < k; c++) {
            ftype cur_distance = 0.0;
            for (int j=0; j < Nf; j++) {
                ftype term = (f[i * Nf + j] - centroids[c * Nf + j]);
                cur_distance += term * term;
            }
            if (best_cluster == -1 || cur_distance < min_distance) {
                min_distance = cur_distance;
                best_cluster = c;
            }
        }
        assignments[i] = 1; //best_cluster;
    }

    return true;
    Py_END_ALLOW_THREADS
}

PyObject* py_assignclass_euclidian(PyObject* self, PyObject* args) {
    try {
        PyArrayObject* fmatrix;
        PyArrayObject* centroids;
        PyArrayObject* assignments;
        if (!PyArg_ParseTuple(args, "OOO", &fmatrix, &centroids, &assignments)) { throw Kmeans_Exception("Wrong number of arguments for assignclass_euclidian."); }
        if (!PyArray_Check(fmatrix) || !PyArray_ISCONTIGUOUS(fmatrix)) throw Kmeans_Exception("fmatrix not what was expected.");
        if (!PyArray_Check(centroids) || !PyArray_ISCONTIGUOUS(centroids)) throw Kmeans_Exception("centroids not what was expected.");
        if (!PyArray_Check(assignments) || !PyArray_ISCONTIGUOUS(assignments)) throw Kmeans_Exception("assignments not what was expected.");
        if (PyArray_TYPE(assignments) != NPY_INT32) throw Kmeans_Exception("assignments should be int32.");
        if (PyArray_TYPE(fmatrix) != PyArray_TYPE(centroids)) throw Kmeans_Exception("centroids and fmatrix should have same type.");
        if (PyArray_NDIM(fmatrix) != 2) throw Kmeans_Exception("fmatrix should be two dimensional");
        if (PyArray_NDIM(centroids) != 2) throw Kmeans_Exception("centroids should be two dimensional");
        if (PyArray_NDIM(assignments) != 1) throw Kmeans_Exception("assignments should be two dimensional");

        const int N = PyArray_DIM(fmatrix, 0);
        const int Nf = PyArray_DIM(fmatrix, 1);
        const int k = PyArray_DIM(centroids, 0);
        if (PyArray_DIM(centroids, 1) != Nf) throw Kmeans_Exception("centroids has wrong number of features.");
        if (PyArray_DIM(assignments, 0) != N) throw Kmeans_Exception("assignments has wrong size.");
        switch (PyArray_TYPE(fmatrix)) {
#define TRY_TYPE_ASSIGN(code, ftype) \
            case code: \
                if (assignclass_euclidian<ftype>( \
                        static_cast<ftype*>(PyArray_DATA(fmatrix)), \
                        static_cast<ftype*>(PyArray_DATA(centroids)), \
                        assignments, \
                        N, Nf, k)) { \
                        Py_RETURN_TRUE; \
                } \
                Py_RETURN_FALSE;

            TRY_TYPE_ASSIGN(NPY_FLOAT, float);
            TRY_TYPE_ASSIGN(NPY_DOUBLE, double);
        }
        throw Kmeans_Exception("Cannot handle this type.");
    } catch (const Kmeans_Exception& exc) {
        PyErr_SetString(PyExc_RuntimeError,exc.msg);
        return 0;
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError,"Some sort of exception in assignclass_euclidian.");
        return 0;
    }
}

template <typename ftype>
int computecentroids(ftype* f, ftype* centroids, PyArrayObject* a_assignments, PyArrayObject* a_counts, const int N, const int Nf, const int k) {
    int zero_counts = 0;
    Py_BEGIN_ALLOW_THREADS
    const npy_int32* assignments = static_cast<npy_int32*>(PyArray_DATA(a_assignments));
    npy_int32* counts = static_cast<npy_int32*>(PyArray_DATA(a_counts));

    std::fill(counts, counts + k, 0);
    std::fill(centroids, centroids + k*Nf, 0.0);

    #pragma omp parallel shared(assignments, counts, centroids, f)
    {
        int *local_counts = (int*) malloc(k * sizeof(int));
        ftype *local_ck = (ftype*) malloc(k * Nf * sizeof(ftype));
        std::fill(local_counts, local_counts + k, 0);
        std::fill(local_ck, local_ck + k*Nf, ftype());

        #pragma omp for schedule(static)
        for (int i = 0; i < N; i++){
            const int c = assignments[i];
            if (c >= k) continue; // throw Kmeans_Exception("wrong value in assignments");
            ftype* lck = local_ck + Nf*c;
            for (int j = 0; j != Nf; ++j) {
                *lck++ += f[i*Nf + j];
            }
            ++local_counts[c];
        }

        ftype* ck = centroids;
        ftype* lck = local_ck;
        for (int c=0; c < k; c++) {
            #pragma omp critical(reduce)
            {
                for (int j = 0; j != Nf; ++j) {
                    *ck++ += *lck++;
                }
                counts[c] += local_counts[c];
            }
        }

        free(local_counts);
        free(local_ck);
    }

    #pragma omp parallel for reduction(+:zero_counts) schedule(static) shared(counts,centroids) 
    for (int i = 0; i < k; ++i) {
        ftype* ck = centroids + Nf*i;
        const ftype c = counts[i];
        if (c == 0) {
            ++zero_counts;
        } else {
            for (int j = 0; j != Nf; ++j) {
                *ck++ /= c;
            }
        }
    }
    Py_END_ALLOW_THREADS
    return zero_counts;
}


PyObject* py_computecentroids(PyObject* self, PyObject* args) {
    try {
        PyArrayObject* fmatrix;
        PyArrayObject* centroids;
        PyArrayObject* assignments;
        PyArrayObject* counts;
        if (!PyArg_ParseTuple(args, "OOOO", &fmatrix, &centroids, &assignments, &counts)) { throw Kmeans_Exception("Wrong number of arguments for computecentroids."); }
        if (!PyArray_Check(fmatrix) || !PyArray_ISCONTIGUOUS(fmatrix)) throw Kmeans_Exception("fmatrix not what was expected.");
        if (!PyArray_Check(centroids) || !PyArray_ISCONTIGUOUS(centroids)) throw Kmeans_Exception("centroids not what was expected.");
        if (!PyArray_Check(counts) || !PyArray_ISCONTIGUOUS(counts)) throw Kmeans_Exception("counts not what was expected.");
        if (!PyArray_Check(assignments) || !PyArray_ISCONTIGUOUS(assignments)) throw Kmeans_Exception("assignments not what was expected.");
        if (PyArray_TYPE(counts) != NPY_INT32) throw Kmeans_Exception("counts should be int32.");
        //if (PyArray_TYPE(assignments) != NPY_INT32) throw Kmeans_Exception("assignments should be int32.");
        if (PyArray_TYPE(fmatrix) != PyArray_TYPE(centroids)) throw Kmeans_Exception("centroids and fmatrix should have same type.");
        if (PyArray_NDIM(fmatrix) != 2) throw Kmeans_Exception("fmatrix should be two dimensional");
        if (PyArray_NDIM(centroids) != 2) throw Kmeans_Exception("centroids should be two dimensional");
        if (PyArray_NDIM(assignments) != 1) throw Kmeans_Exception("assignments should be two dimensional");

        const int N = PyArray_DIM(fmatrix, 0);
        const int Nf = PyArray_DIM(fmatrix, 1);
        const int k = PyArray_DIM(centroids, 0);
        if (PyArray_DIM(centroids, 1) != Nf) throw Kmeans_Exception("centroids has wrong number of features.");
        if (PyArray_DIM(assignments, 0) != N) throw Kmeans_Exception("assignments has wrong size.");
        if (PyArray_DIM(counts, 0) != k) throw Kmeans_Exception("counts has wrong size.");
        switch (PyArray_TYPE(fmatrix)) {
#define TRY_TYPE_CENTROIDS(code, ftype) \
            case code: \
                if (computecentroids<ftype>( \
                        static_cast<ftype*>(PyArray_DATA(fmatrix)), \
                        static_cast<ftype*>(PyArray_DATA(centroids)), \
                        assignments, \
                        counts, \
                        N, Nf, k)) { \
                        Py_RETURN_TRUE; \
                } \
                Py_RETURN_FALSE;

            TRY_TYPE_CENTROIDS(NPY_FLOAT, float);
            TRY_TYPE_CENTROIDS(NPY_DOUBLE, double);
        }
        throw Kmeans_Exception("Cannot handle this type.");
    } catch (const Kmeans_Exception& exc) {
        PyErr_SetString(PyExc_RuntimeError,exc.msg);
        return 0;
    } catch (...) {
        PyErr_SetString(PyExc_RuntimeError,"Some sort of exception in computecentroids.");
        return 0;
    }
}

PyMethodDef methods[] = {
  {"computecentroids", py_computecentroids, METH_VARARGS , "Do NOT call directly.\n" },
  {"assignclass_euclidian", py_assignclass_euclidian, METH_VARARGS , "Do NOT call directly.\n" },
  {NULL, NULL,0,NULL},
};

const char  * module_doc = 
    "Internal _kmeans Module.\n"
    "\n"
    "Do NOT use directly!\n";

} // namespace
extern "C"
void init_kmeans()
  {
    import_array();
    (void)Py_InitModule3("_kmeans", methods, module_doc);
  }

