if(CMAKE_COMPILER_IS_GNUCC)
  add_compile_options(-ffast-math)
endif()

app(delaunayrefinement DelaunayRefinement.cpp)

add_test_scale(small1 delaunayrefinement "${BASEINPUT}/reference/meshes/r10k.1")
add_test_scale(small2 delaunayrefinement "${BASEINPUT}/meshes/250k.2" NOT_QUICK)
