/**
 * \example ex.11.simple-stencil/main.cpp
 *
 * Stencil codes are iterative kernels on arrays of at least 2 dimensions
 * where the value of an array element at iteration i+1 depends on the values
 * of its neighbors in iteration i.
 *
 * Calculations of this kind are very common in scientific applications, e.g.
 * in iterative solvers and filters in image processing.
 *
 * This example implements a very simple blur filter. For simplicity
 * no real image is used, but an image containg circles is generated.
 * 
 * \todo fix \c dash::copy problem
 */

#include <dash/Init.h>
#include <dash/Matrix.h>
#include <dash/Types.h>
#include <dash/Dimensional.h>
#include <dash/TeamSpec.h>
#include <dash/algorithm/Fill.h>
#include <dash/util/Timer.h>

#include <fstream>
#include <string>
#include <iostream>
#include <vector>

// required for tasking abstraction
#include <functional>
#include <array>
#include <dash/dart/if/dart.h>

using namespace std;

using element_t = unsigned char;
using Array_t   = dash::NArray<element_t, 2>;
using index_t = typename Array_t::index_type;


namespace dash{

namespace internal {

  using FuncT = std::function<void()>;

  //template<typename FuncT>
  static void invoke_task_action(void *data)
  {
    FuncT *func = static_cast<FuncT*>(data);
    func->operator()();
    delete func;
  }

} // namespace internal

  template<typename ElementT>
  dart_task_dep_t
  in(dash::GlobRef<ElementT> globref, int32_t epoch = DART_EPOCH_ANY) {
    dart_task_dep_t res;
    res.gptr  = globref.dart_gptr();
    res.type  = DART_DEP_IN;
    res.epoch = epoch;
    return res;
  }

  template<typename ContainerT, typename ElementT>
  dart_task_dep_t
  in(ContainerT& container, ElementT* lptr, int32_t epoch = DART_EPOCH_ANY) {
    dart_task_dep_t res;
    res.gptr  = container.begin().dart_gptr();
    dart_gptr_incaddr(&res.gptr, lptr - container.lbegin());
    res.type  = DART_DEP_IN;
    res.epoch = epoch;
    return res;
  }

  template<typename ElementT>
  dart_task_dep_t
  out(dash::GlobRef<ElementT> globref, int32_t epoch = DART_EPOCH_ANY) {
    dart_task_dep_t res;
    res.gptr  = globref.dart_gptr();
    res.type  = DART_DEP_OUT;
    res.epoch = epoch;
    return res;
  }

  template<typename ContainerT, typename ElementT>
  dart_task_dep_t
  out(ContainerT& container, ElementT* lptr, int32_t epoch = DART_EPOCH_ANY) {
    dart_task_dep_t res;
    res.gptr  = container.begin().dart_gptr();
    dart_gptr_incaddr(&res.gptr, lptr - container.lbegin());
    res.type  = DART_DEP_OUT;
    res.epoch = epoch;
    return res;
  }

  dart_task_dep_t
  direct(dart_taskref_t taskref) {
    dart_task_dep_t res;
    res.task  = taskref;
    res.type  = DART_DEP_DIRECT;
    return res;
  }

  template<class TaskFunc, typename ... Args>
  void
  create_task(TaskFunc f, const Args&... args){
    std::array<dart_task_dep_t, sizeof...(args)> deps({{
      static_cast<dart_task_dep_t>(args)...
    }});
    dart_task_create(
        &dash::internal::invoke_task_action,
        new dash::internal::FuncT(f), 0,
        deps.data(), deps.size());
  }


  template<class TaskFunc, typename ... Args>
  dart_taskref_t
  create_task_handle(TaskFunc f, const Args&... args){
    std::array<dart_task_dep_t, sizeof...(args)> deps({{
      static_cast<dart_task_dep_t>(args)...
    }});
    dart_task_create(
        &dash::internal::invoke_task_action,
        new dash::internal::FuncT(f), 0,
        deps.data(), deps.size());
  }

} // namespace dash


void write_pgm(const std::string & filename, const Array_t & data){
  if(dash::myid() == 0){

    auto ext_x = data.extent(0);
    auto ext_y = data.extent(1);
    std::ofstream file;
    file.open(filename);

    file << "P2\n" << ext_x << " " << ext_y << "\n"
         << "255" << std::endl;
    // Data
//    std::vector<element_t> buffer(ext_x);

    for(long x=0; x<ext_x; ++x){
//      auto first = data.begin()+ext_y*x;
//      auto last  = data.begin()+(ext_y*(x+1));

//      BUG!!!!
//      dash::copy(first, last, buffer.data());

      for(long y=0; y<ext_y; ++y){
//        file << buffer[x] << " ";
        file << setfill(' ') << setw(3)
             << static_cast<int>(data[x][y]) << " ";
      }
      file << std::endl;
    }
    file.close();
  }
  dash::barrier();
}

void set_pixel(Array_t & data, index_t x, index_t y){
  const element_t color = 1;
  auto ext_x = data.extent(0);
  auto ext_y = data.extent(1);

  x = (x+ext_x)%ext_x;
  y = (y+ext_y)%ext_y;

  data.at(x, y) = color;
}

void draw_circle(Array_t & data, index_t x0, index_t y0, int r){
  // Check who owns center, owner draws
  if(!data.at(x0, y0).is_local()){
    return;
  }

  int       f     = 1-r;
  int       ddF_x = 1;
  int       ddF_y = -2*r;
  index_t   x     = 0;
  index_t   y     = r;

  set_pixel(data, x0 - r, y0);
  set_pixel(data, x0 + r, y0);
  set_pixel(data, x0, y0 - r);
  set_pixel(data, x0, y0 + r);

  while(x<y){
    if(f>=0){
      y--;
      ddF_y+=2;
      f+=ddF_y;
    }
    ++x;
    ddF_x+=2;
    f+=ddF_x;
    set_pixel(data, x0+x, y0+y);
    set_pixel(data, x0-x, y0+y);
    set_pixel(data, x0+x, y0-y);
    set_pixel(data, x0-x, y0-y);
    set_pixel(data, x0+y, y0+x);
    set_pixel(data, x0-y, y0+x);
    set_pixel(data, x0+y, y0-x);
    set_pixel(data, x0-y, y0-x);
  }
}

void smooth(Array_t & data_old, Array_t & data_new, int32_t iter){
  // Todo: use stencil iterator
  const auto& pattern = data_old.pattern();

  auto gext_x = data_old.extent(0);
  auto gext_y = data_old.extent(1);

  auto lext_x = pattern.local_extent(0);
  auto lext_y = pattern.local_extent(1);
  auto local_beg_gidx = pattern.coords(pattern.global(0));
  auto local_end_gidx = pattern.coords(pattern.global(pattern.local_size()-1));

  // Inner rows
  for( index_t x=1; x<lext_x-1; x++ ) {
    dash::create_task(
      [=, &data_old, &data_new] {
//        std::cout << "Computing row " << x << " in iteration " << iter << std::endl;
      const element_t *__restrict curr_row = data_old.local.row(x).lbegin();
      const element_t *__restrict   up_row = data_old.local.row(x-1).lbegin();
      const element_t *__restrict down_row = data_old.local.row(x+1).lbegin();
            element_t *__restrict  out_row = data_new.local.row(x).lbegin();
//        const element_t* curr_row = data_old.local[x  ].lbegin();
//        const element_t*   up_row = data_old.local[x-1].lbegin();
//        const element_t* down_row = data_old.local[x+1].lbegin();
//              element_t*  out_row = data_new.local[x  ].lbegin();
        for( index_t y=1; y<lext_y-1; y++ ) {
          out_row[y] =
            ( 0.40 * curr_row[y] +
              0.15 * curr_row[y-1] +
              0.15 * curr_row[y+1] +
              0.15 * down_row[y] +
              0.15 *   up_row[y]);
        }
      },
      // use the first element in the row as sentinel
      dash::in(data_old.at(local_beg_gidx[0] + x, 0), iter-1),
      dash::in(data_old.at(local_beg_gidx[0] + x+1, 0), iter-1),
      dash::in(data_old.at(local_beg_gidx[0] + x-1, 0), iter-1),
      dash::out(data_new.at(local_beg_gidx[0] + x, 0), iter)
    );
#if DEBUG
    printf("[%d] MIDDLE in %d: (%d [%p]), (%d [%p]), (%d [%p]); out %d: (%d [%p])\n", 
           dash::myid().id, iter-1,  local_beg_gidx[0] + x, data_old[local_beg_gidx[0] + x].begin().local(),
           local_beg_gidx[0] + x - 1, data_old[local_beg_gidx[0] + x-1].begin().local(),
           local_beg_gidx[0] + x + 1, data_old[local_beg_gidx[0] + x+1].begin().local(),
           iter, local_beg_gidx[0] + x, data_new[local_beg_gidx[0] + x].begin().local());
#endif
  }

  // Boundary
  index_t begin_idx_x = (local_beg_gidx[0] == 0) ? 1 : 0;
  index_t end_idx_x   = (local_end_gidx[0] == gext_x-1) ? lext_x-2 : lext_x-1;
  index_t begin_idx_y = (local_beg_gidx[1] == 0) ? 1 : 0;
  index_t end_idx_y   = (local_end_gidx[1] == gext_y-1) ? lext_y-2 : lext_y-1;
  bool is_top    =(local_beg_gidx[0] == 0) ? true : false;
  bool is_bottom =(local_end_gidx[0] == (gext_x-1)) ? true : false;

  if(!is_top){
    // top row
    dash::create_task(
     [=, &data_old, &data_new] {
      const element_t *__restrict down_row = data_old.local.row(1).lbegin();
      const element_t *__restrict curr_row = data_old.local.row(0).lbegin();
            element_t *__restrict  out_row = data_new.lbegin();
            element_t *__restrict   up_row = static_cast<element_t*>(
                                    std::malloc(sizeof(element_t) * gext_y));
        // copy line
        // TODO: make this non-blocking and yield
        dart_get_blocking(
          up_row,
          data_old[local_beg_gidx[0] - 1][0].dart_gptr(),
          gext_y, dash::dart_datatype<element_t>::value);
        for( auto y=1; y<gext_y-1; ++y){
          out_row[y] =
            ( 0.40 * curr_row[y] +
              0.15 *   up_row[y] +
              0.15 * down_row[y] +
              0.15 * curr_row[y-1] +
              0.15 * curr_row[y+1]);
        }
        std::free(up_row);
      },
      dash::in(data_old.at(local_beg_gidx[0] - 1,   0), iter-1),
      dash::in(data_old.at(local_beg_gidx[0] + 1,   0), iter-1),
      dash::in(data_old.at(local_beg_gidx[0],       0), iter-1),
      dash::out(data_new.at(local_beg_gidx[0],      0), iter)
    );
#if DEBUG
    printf("[%d] TOP    in %d: (%d), (%d), (%d); out %d: (%d)\n", 
           dash::myid().id, iter-1, local_beg_gidx[0] - 1, 
           local_beg_gidx[0] + 1, local_beg_gidx[0], iter, local_beg_gidx[0]);
#endif
  }

  if(!is_bottom){
    // bottom row
    auto handle = dash::create_task_handle(
      [=, &data_old, &data_new] {
        if (dash::myid() == 0)
          std::cout << "[0] Computing bottom row in iteration " << iter << std::endl;
        const element_t *__restrict   up_row = data_old[local_end_gidx[0] - 1].begin().local();
        const element_t *__restrict curr_row = data_old[local_end_gidx[0]].begin().local();
              element_t *__restrict down_row = static_cast<element_t*>(
                                      std::malloc(sizeof(element_t) * gext_y));
              element_t *__restrict  out_row = data_new[local_end_gidx[0]].begin().local();
        // copy line
        // TODO: make this non-blocking and yield
        dart_get_blocking(
          down_row,
          data_old[local_end_gidx[0] + 1].begin().dart_gptr(),
          gext_y, dash::dart_datatype<element_t>::value);
        for( auto y=1; y<gext_y-1; ++y){
          out_row[y] =
            ( 0.40 * curr_row[y] +
              0.15 *   up_row[y] +
              0.15 * down_row[y] +
              0.15 * curr_row[y-1] +
              0.15 * curr_row[y+1]);
        }
        std::free(down_row);
      },
      dash::in(data_old.at(local_end_gidx[0] - 1,   0), iter-1),
      dash::in(data_old.at(local_end_gidx[0] + 1,   0), iter-1),
      dash::in(data_old.at(local_end_gidx[0],       0), iter-1),
      dash::out(data_new.at(local_end_gidx[0],      0), iter)
    );
#if DEBUG
    printf("[%d] BOTTOM in %d: (%d [%p]), (%d [-]), (%d [%p]); out %d: (%d [%p]) \n", 
           dash::myid().id, iter-1, local_end_gidx[0] - 1, data_old[local_end_gidx[0] - 1].begin().local(),
           local_end_gidx[0] + 1,
           local_end_gidx[0], data_old[local_end_gidx[0]].begin().local(), 
           iter, local_end_gidx[0], data_new[local_end_gidx[0]].begin().local());
#endif
  }
}

int main(int argc, char* argv[])
{
  int sizex = 1000;
  int sizey = 1000;
  int niter = 100;
  typedef dash::util::Timer<
    dash::util::TimeMeasure::Clock
  > Timer;

  dash::init(&argc, &argv);

  Timer::Calibrate(0);

  // Prepare grid
  dash::TeamSpec<2> ts;
  dash::SizeSpec<2> ss(sizex, sizey);
  dash::DistributionSpec<2> ds(dash::BLOCKED, dash::NONE);
//  ts.balance_extents();

  dash::Pattern<2> pattern(ss, ds, ts);

  Array_t data_old(pattern);
  Array_t data_new(pattern);

  auto gextents =  data_old.pattern().extents();
  auto lextents =  data_old.pattern().local_extents();
  std::cout << "Global extents: " << gextents[0] << "," << gextents[1] << std::endl;
  std::cout << "Local extents: "  << lextents[0] << "," << lextents[1] << std::endl;

  dash::fill(data_old.begin(), data_old.end(), 255);
  dash::fill(data_new.begin(), data_new.end(), 255);

  if (sizex > 400) {
    draw_circle(data_old, 0, 0, 40);
    draw_circle(data_old, 0, 0, 30);
    draw_circle(data_old, 200, 100, 10);
    draw_circle(data_old, 200, 100, 20);
    draw_circle(data_old, 200, 100, 30);
    draw_circle(data_old, 200, 100, 40);
    draw_circle(data_old, 200, 100, 50);
  }

  if (sizex >= 1000) {
    draw_circle(data_old, sizex / 4, sizey / 4, sizex / 100);
    draw_circle(data_old, sizex / 4, sizey / 4, sizex /  50);
    draw_circle(data_old, sizex / 4, sizey / 4, sizex /  33);
    draw_circle(data_old, sizex / 4, sizey / 4, sizex /  25);
    draw_circle(data_old, sizex / 4, sizey / 4, sizex /  20);

    draw_circle(data_old, sizex / 2, sizey / 2, sizex / 100);
    draw_circle(data_old, sizex / 2, sizey / 2, sizex /  50);
    draw_circle(data_old, sizex / 2, sizey / 2, sizex /  33);
    draw_circle(data_old, sizex / 2, sizey / 2, sizex /  25);
    draw_circle(data_old, sizex / 2, sizey / 2, sizex /  20);

    draw_circle(data_old, sizex / 4 * 3, sizey / 4 * 3, sizex / 100);
    draw_circle(data_old, sizex / 4 * 3, sizey / 4 * 3, sizex /  50);
    draw_circle(data_old, sizex / 4 * 3, sizey / 4 * 3, sizex /  33);
    draw_circle(data_old, sizex / 4 * 3, sizey / 4 * 3, sizex /  25);
    draw_circle(data_old, sizex / 4 * 3, sizey / 4 * 3, sizex /  20);
  }
  dash::barrier();

  if (sizex <= 1000)
    write_pgm("testimg_input.pgm", data_old);

  Timer timer;

  for(int i=0; i<niter; ++i){
    // switch references
    auto & data_prev = i%2 ? data_new : data_old;
    auto & data_next = i%2 ? data_old : data_new;

    std::cout << "Creating tasks for iteration " << i << std::endl;
    smooth(data_prev, data_next, i+1);
  }
//  dash::barrier();
  std::cout << "Done creating tasks, starting computation" << std::endl;
  dart_task_complete();
  dash::barrier();
  if (dash::myid() == 0) {
    std::cout << "Done computing (" << timer.Elapsed() / 1E6 << "s)" << std::endl;
  }

  if (sizex <= 1000)
    write_pgm("testimg_output.pgm", data_new);
  dash::finalize();
}