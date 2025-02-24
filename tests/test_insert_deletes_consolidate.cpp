// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <index.h>
#include <numeric>
#include <omp.h>
#include <string.h>
#include <time.h>
#include <timer.h>
#include <boost/program_options.hpp>
#include <future>

#include "utils.h"

#ifndef _WINDOWS
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "memory_mapper.h"

namespace po = boost::program_options;

// load_aligned_bin modified to read pieces of the file, but using ifstream
// instead of cached_ifstream.
template<typename T>
inline void load_aligned_bin_part(const std::string& bin_file, T* data,
                                  size_t offset_points, size_t points_to_read) {
  diskann::Timer timer;
  std::ifstream  reader;
  reader.exceptions(std::ios::failbit | std::ios::badbit);
  reader.open(bin_file, std::ios::binary | std::ios::ate);
  size_t actual_file_size = reader.tellg();
  reader.seekg(0, std::ios::beg);

  int npts_i32, dim_i32;
  reader.read((char*) &npts_i32, sizeof(int));
  reader.read((char*) &dim_i32, sizeof(int));
  size_t npts = (unsigned) npts_i32;
  size_t dim = (unsigned) dim_i32;

  size_t expected_actual_file_size =
      npts * dim * sizeof(T) + 2 * sizeof(uint32_t);
  if (actual_file_size != expected_actual_file_size) {
    std::stringstream stream;
    stream << "Error. File size mismatch. Actual size is " << actual_file_size
           << " while expected size is  " << expected_actual_file_size
           << " npts = " << npts << " dim = " << dim
           << " size of <T>= " << sizeof(T) << std::endl;
    std::cout << stream.str();
    throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__,
                                __LINE__);
  }

  if (offset_points + points_to_read > npts) {
    std::stringstream stream;
    stream << "Error. Not enough points in file. Requested " << offset_points
           << "  offset and " << points_to_read << " points, but have only "
           << npts << " points" << std::endl;
    std::cout << stream.str();
    throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__,
                                __LINE__);
  }

  reader.seekg(2 * sizeof(uint32_t) + offset_points * dim * sizeof(T));

  const size_t rounded_dim = ROUND_UP(dim, 8);

  for (size_t i = 0; i < points_to_read; i++) {
    reader.read((char*) (data + i * rounded_dim), dim * sizeof(T));
    memset(data + i * rounded_dim + dim, 0, (rounded_dim - dim) * sizeof(T));
  }
  reader.close();

  const double elapsedSeconds = timer.elapsed() / 1000000.0;
  std::cout << "Read " << points_to_read << " points using non-cached reads in "
            << elapsedSeconds << std::endl;
}

std::string get_save_filename(const std::string& save_path,
                              size_t points_to_skip, size_t points_deleted,
                              size_t last_point_threshold) {
  std::string final_path = save_path;
  if (points_to_skip > 0) {
    final_path += "skip" + std::to_string(points_to_skip) + "-";
  }

  final_path += "del" + std::to_string(points_deleted) + "-";
  final_path += std::to_string(last_point_threshold);
  return final_path;
}

template<typename T, typename TagT>
void insert_till_next_checkpoint(diskann::Index<T, TagT>& index, size_t start,
                                 size_t end, size_t thread_count, T* data,
                                 size_t aligned_dim) {
  diskann::Timer insert_timer;

#pragma omp parallel for num_threads(thread_count) schedule(dynamic)
  for (int64_t j = start; j < (int64_t) end; j++) {
    index.insert_point(&data[(j - start) * aligned_dim],
                       1 + static_cast<TagT>(j));
  }
  const double elapsedSeconds = insert_timer.elapsed() / 1000000.0;
  std::cout << "Insertion time " << elapsedSeconds << " seconds ("
            << (end - start) / elapsedSeconds << " points/second overall, "
            << (end - start) / elapsedSeconds / thread_count
            << " per thread)\n ";
}

template<typename T, typename TagT>
void delete_from_beginning(diskann::Index<T, TagT>& index,
                           diskann::Parameters&     delete_params,
                           size_t                   points_to_skip,
                           size_t points_to_delete_from_beginning) {
  try {
    std::cout << std::endl
              << "Lazy deleting points " << points_to_skip << " to "
              << points_to_skip + points_to_delete_from_beginning << "... ";
    for (size_t i = points_to_skip;
         i < points_to_skip + points_to_delete_from_beginning; ++i)
      index.lazy_delete(i + 1);  // Since tags are data location + 1
    std::cout << "done." << std::endl;

    auto report = index.consolidate_deletes(delete_params);
    std::cout << "#active points: " << report._active_points << std::endl
              << "max points: " << report._max_points << std::endl
              << "empty slots: " << report._empty_slots << std::endl
              << "deletes processed: " << report._slots_released << std::endl
              << "latest delete size: " << report._delete_set_size << std::endl
              << "rate: (" << points_to_delete_from_beginning / report._time
              << " points/second overall, "
              << points_to_delete_from_beginning / report._time /
                     delete_params.Get<unsigned>("num_threads")
              << " per thread)" << std::endl;
  } catch (std::system_error& e) {
    std::cout << "Exception caught in deletion thread: " << e.what()
              << std::endl;
  }
}

template<typename T>
void build_incremental_index(
    const std::string& data_path, const unsigned L, const unsigned R,
    const float alpha, const unsigned thread_count, size_t points_to_skip,
    size_t max_points_to_insert, size_t beginning_index_size,
    float start_point_norm, size_t points_per_checkpoint,
    size_t checkpoints_per_snapshot, const std::string& save_path,
    size_t points_to_delete_from_beginning, size_t start_deletes_after,
    bool concurrent) {
  const unsigned C = 500;
  const bool     saturate_graph = false;

  diskann::Parameters params;
  params.Set<unsigned>("L", L);
  params.Set<unsigned>("R", R);
  params.Set<unsigned>("C", C);
  params.Set<float>("alpha", alpha);
  params.Set<bool>("saturate_graph", saturate_graph);
  params.Set<unsigned>("num_rnds", 1);
  params.Set<unsigned>("num_threads", thread_count);

  size_t dim, aligned_dim;
  size_t num_points;

  diskann::get_bin_metadata(data_path, num_points, dim);
  aligned_dim = ROUND_UP(dim, 8);

  if (points_to_skip > num_points) {
    throw diskann::ANNException("Asked to skip more points than in data file",
                                -1, __FUNCSIG__, __FILE__, __LINE__);
  }

  if (max_points_to_insert == 0) {
    max_points_to_insert = num_points;
  }

  if (points_to_skip + max_points_to_insert > num_points) {
    max_points_to_insert = num_points - points_to_skip;
    std::cerr << "WARNING: Reducing max_points_to_insert to "
              << max_points_to_insert
              << " points since the data file has only that many" << std::endl;
  }

  using TagT = uint32_t;
  unsigned   num_frozen = 1;
  const bool enable_tags = true;

  auto num_frozen_str = getenv("TTS_NUM_FROZEN");

  if (num_frozen_str != nullptr) {
    num_frozen = std::atoi(num_frozen_str);
    std::cout << "Overriding num_frozen to" << num_frozen << std::endl;
  }

  diskann::Index<T, TagT> index(diskann::L2, dim, max_points_to_insert, true,
                                params, params, enable_tags, concurrent);

  size_t       current_point_offset = points_to_skip;
  const size_t last_point_threshold = points_to_skip + max_points_to_insert;

  if (beginning_index_size > max_points_to_insert) {
    beginning_index_size = max_points_to_insert;
    std::cerr << "WARNING: Reducing beginning index size to "
              << beginning_index_size
              << " points since the data file has only that many" << std::endl;
  }
  if (checkpoints_per_snapshot > 0 &&
      beginning_index_size > points_per_checkpoint) {
    beginning_index_size = points_per_checkpoint;
    std::cerr << "WARNING: Reducing beginning index size to "
              << beginning_index_size << std::endl;
  }

  T* data = nullptr;
  diskann::alloc_aligned((void**) &data,
                         std::max(points_per_checkpoint, beginning_index_size) *
                             aligned_dim * sizeof(T),
                         8 * sizeof(T));

  std::vector<TagT> tags(beginning_index_size);
  std::iota(tags.begin(), tags.end(),
            1 + static_cast<TagT>(current_point_offset));

  load_aligned_bin_part(data_path, data, current_point_offset,
                        beginning_index_size);
  std::cout << "load aligned bin succeeded" << std::endl;
  diskann::Timer timer;

  if (beginning_index_size > 0) {
    index.build(data, beginning_index_size, params, tags);
    index.enable_delete();
  } else {
    index.set_start_point_at_random(static_cast<T>(start_point_norm));
    index.enable_delete();
  }

  const double elapsedSeconds = timer.elapsed() / 1000000.0;
  std::cout << "Initial non-incremental index build time for "
            << beginning_index_size << " points took " << elapsedSeconds
            << " seconds (" << beginning_index_size / elapsedSeconds
            << " points/second)\n ";

  current_point_offset = beginning_index_size;

  if (points_to_delete_from_beginning > max_points_to_insert) {
    points_to_delete_from_beginning =
        static_cast<unsigned>(max_points_to_insert);
    std::cerr << "WARNING: Reducing points to delete from beginning to "
              << points_to_delete_from_beginning
              << " points since the data file has only that many" << std::endl;
  }

  if (concurrent) {
    int               sub_threads = (thread_count + 1) / 2;
    bool              delete_launched = false;
    std::future<void> delete_task;

    diskann::Timer timer;

    for (size_t start = current_point_offset; start < last_point_threshold;
         start += points_per_checkpoint,
                current_point_offset += points_per_checkpoint) {
      const size_t end =
          std::min(start + points_per_checkpoint, last_point_threshold);
      std::cout << std::endl
                << "Inserting from " << start << " to " << end << std::endl;

      auto insert_task = std::async(std::launch::async, [&]() {
        load_aligned_bin_part(data_path, data, start, end - start);
        insert_till_next_checkpoint(index, start, end, sub_threads, data,
                                    aligned_dim);
      });
      insert_task.wait();

      if (!delete_launched && end >= start_deletes_after &&
          end >= points_to_skip + points_to_delete_from_beginning) {
        delete_launched = true;
        params.Set<unsigned>("num_threads", sub_threads);

        delete_task = std::async(std::launch::async, [&]() {
          delete_from_beginning(index, params, points_to_skip,
                                points_to_delete_from_beginning);
        });
      }
    }
    delete_task.wait();

    std::cout << "Time Elapsed " << timer.elapsed() / 1000 << "ms\n";
    const auto save_path_inc = get_save_filename(
        save_path + ".after-concurrent-delete-", points_to_skip,
        points_to_delete_from_beginning, last_point_threshold);
    index.save(save_path_inc.c_str(), true);

  } else {
    size_t last_snapshot_points_threshold = 0;
    size_t num_checkpoints_till_snapshot = checkpoints_per_snapshot;

    for (size_t start = current_point_offset; start < last_point_threshold;
         start += points_per_checkpoint,
                current_point_offset += points_per_checkpoint) {
      const size_t end =
          std::min(start + points_per_checkpoint, last_point_threshold);
      std::cout << std::endl
                << "Inserting from " << start << " to " << end << std::endl;

      load_aligned_bin_part(data_path, data, start, end - start);
      insert_till_next_checkpoint(index, start, end, thread_count, data,
                                  aligned_dim);

      if (checkpoints_per_snapshot > 0 &&
          --num_checkpoints_till_snapshot == 0) {
        diskann::Timer save_timer;

        const auto save_path_inc =
            get_save_filename(save_path + ".inc-", points_to_skip,
                              points_to_delete_from_beginning, end);
        index.save(save_path_inc.c_str(), false);
        const double elapsedSeconds = save_timer.elapsed() / 1000000.0;
        const size_t points_saved = end - points_to_skip;

        std::cout << "Saved " << points_saved << " points in " << elapsedSeconds
                  << " seconds (" << points_saved / elapsedSeconds
                  << " points/second)\n";

        num_checkpoints_till_snapshot = checkpoints_per_snapshot;
        last_snapshot_points_threshold = end;
      }

      std::cout << "Number of points in the index post insertion " << end
                << std::endl;
    }

    if (checkpoints_per_snapshot > 0 &&
        last_snapshot_points_threshold != last_point_threshold) {
      const auto save_path_inc = get_save_filename(
          save_path + ".inc-", points_to_skip, points_to_delete_from_beginning,
          last_point_threshold);
      // index.save(save_path_inc.c_str(), false);
    }

    if (points_to_delete_from_beginning > 0) {
      delete_from_beginning(index, params, points_to_skip,
                            points_to_delete_from_beginning);
    }
    const auto save_path_inc = get_save_filename(
        save_path + ".after-delete-", points_to_skip,
        points_to_delete_from_beginning, last_point_threshold);
    index.save(save_path_inc.c_str(), true);
  }

  diskann::aligned_free(data);
}

int main(int argc, char** argv) {
  std::string data_type, dist_fn, data_path, index_path_prefix;
  unsigned    num_threads, R, L;
  float       alpha, start_point_norm;
  size_t      points_to_skip, max_points_to_insert, beginning_index_size,
      points_per_checkpoint, checkpoints_per_snapshot,
      points_to_delete_from_beginning, start_deletes_after;
  bool concurrent;

  po::options_description desc{"Arguments"};
  try {
    desc.add_options()("help,h", "Print information on arguments");
    desc.add_options()("data_type",
                       po::value<std::string>(&data_type)->required(),
                       "data type <int8/uint8/float>");
    desc.add_options()("dist_fn", po::value<std::string>(&dist_fn)->required(),
                       "distance function <l2/mips>");
    desc.add_options()("data_path",
                       po::value<std::string>(&data_path)->required(),
                       "Input data file in bin format");
    desc.add_options()("index_path_prefix",
                       po::value<std::string>(&index_path_prefix)->required(),
                       "Path prefix for saving index file components");
    desc.add_options()("max_degree,R",
                       po::value<uint32_t>(&R)->default_value(64),
                       "Maximum graph degree");
    desc.add_options()(
        "Lbuild,L", po::value<uint32_t>(&L)->default_value(100),
        "Build complexity, higher value results in better graphs");
    desc.add_options()(
        "alpha", po::value<float>(&alpha)->default_value(1.2f),
        "alpha controls density and diameter of graph, set 1 for sparse graph, "
        "1.2 or 1.4 for denser graphs with lower diameter");
    desc.add_options()(
        "num_threads,T",
        po::value<uint32_t>(&num_threads)->default_value(omp_get_num_procs()),
        "Number of threads used for building index (defaults to "
        "omp_get_num_procs())");
    desc.add_options()("points_to_skip",
                       po::value<uint64_t>(&points_to_skip)->required(),
                       "Skip these first set of points from file");
    desc.add_options()(
        "max_points_to_insert",
        po::value<uint64_t>(&max_points_to_insert)->default_value(0),
        "These number of points from the file are inserted after "
        "points_to_skip");
    desc.add_options()("beginning_index_size",
                       po::value<uint64_t>(&beginning_index_size)->required(),
                       "Batch build will be called on these set of points");
    desc.add_options()(
        "points_per_checkpoint",
        po::value<uint64_t>(&points_per_checkpoint)->required(),
        "Insertions are done in batches of points_per_checkpoint");
    desc.add_options()(
        "checkpoints_per_snapshot",
        po::value<uint64_t>(&checkpoints_per_snapshot)->required(),
        "Save the index to disk every few checkpoints");
    desc.add_options()(
        "points_to_delete_from_beginning",
        po::value<uint64_t>(&points_to_delete_from_beginning)->required(), "");
    desc.add_options()("do_concurrent",
                       po::value<bool>(&concurrent)->default_value(false), "");
    desc.add_options()(
        "start_deletes_after",
        po::value<uint64_t>(&start_deletes_after)->default_value(0), "");
    desc.add_options()(
        "start_point_norm",
        po::value<float>(&start_point_norm)->default_value(0),
        "Set the start point to a random point on a sphere of this radius");

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help")) {
      std::cout << desc;
      return 0;
    }
    po::notify(vm);
    if (beginning_index_size == 0)
      if (start_point_norm == 0) {
        std::cout << "When beginning_index_size is 0, use a start point with  "
                     "appropriate norm"
                  << std::endl;
        return -1;
      }
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return -1;
  }

  try {
    if (data_type == std::string("int8"))
      build_incremental_index<int8_t>(
          data_path, L, R, alpha, num_threads, points_to_skip,
          max_points_to_insert, beginning_index_size, start_point_norm,
          points_per_checkpoint, checkpoints_per_snapshot, index_path_prefix,
          points_to_delete_from_beginning, start_deletes_after, concurrent);
    else if (data_type == std::string("uint8"))
      build_incremental_index<uint8_t>(
          data_path, L, R, alpha, num_threads, points_to_skip,
          max_points_to_insert, beginning_index_size, start_point_norm,
          points_per_checkpoint, checkpoints_per_snapshot, index_path_prefix,
          points_to_delete_from_beginning, start_deletes_after, concurrent);
    else if (data_type == std::string("float"))
      build_incremental_index<float>(
          data_path, L, R, alpha, num_threads, points_to_skip,
          max_points_to_insert, beginning_index_size, start_point_norm,
          points_per_checkpoint, checkpoints_per_snapshot, index_path_prefix,
          points_to_delete_from_beginning, start_deletes_after, concurrent);
    else
      std::cout << "Unsupported type. Use float/int8/uint8" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Caught exception: " << e.what() << std::endl;
    exit(-1);
  } catch (...) {
    std::cerr << "Caught unknown exception" << std::endl;
    exit(-1);
  }

  return 0;
}
