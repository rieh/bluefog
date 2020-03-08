#include "mpi_controller.h"

#include <algorithm>
#include <cassert>
#include <thread>

namespace bluefog {
namespace common {

// MPIController
void MPIController::Initialize() {
  // Check if multi-thread is supported.
  int provided;
  MPI_Query_thread(&provided);
  mpi_threads_supported_ = (provided == MPI_THREAD_MULTIPLE);

  // Get MPI rank to determine if we are rank zero.
  MPI_Comm_rank(mpi_ctx_.mpi_comm, &rank_);

  // Get MPI size to determine how many tensors to wait for before reducing.
  MPI_Comm_size(mpi_ctx_.mpi_comm, &size_);

  // Determine local rank by querying the local communicator.
  MPI_Comm_rank(mpi_ctx_.local_comm, &local_rank_);
  MPI_Comm_size(mpi_ctx_.local_comm, &local_size_);
  local_comm_ranks_ = std::vector<int>((size_t)local_size_);
  local_comm_ranks_[local_rank_] = rank_;

  // Get cross-node rank and size in case of hierarchical allreduce.
  MPI_Comm_rank(mpi_ctx_.cross_comm, &cross_rank_);
  MPI_Comm_size(mpi_ctx_.cross_comm, &cross_size_);

  BFLOG(DEBUG) << "MPI controller initialized.";
}

int MPIController::GetTypeSize(DataType dtype) {
  return mpi_ctx_.GetMPITypeSize(dtype);
}

Status MPIController::AllocateOutput(TensorTableEntry& entry, int*& recvcounts,
                                     Communicator comm_type) {
  // Every tensor participating in Allgather operation may have different
  // first dimension size, but the rest of dimensions are same for all
  // tensors.  Here we get shape of tensor sliced by first dimension.
  // TODO(ybc): Check single_slice_shape is same cross all ranks.
  TensorShape single_slice_shape;
  for (int i = 1; i < entry.tensor->shape().dims(); ++i) {
    single_slice_shape.AddDim(entry.tensor->shape().dim_size(i));
  }

  // cnt_size is the number of expected receiving info.
  // For allgather, it is the global size.
  // For neighbor_allgather, it is the number of in-neighbor (excluding itself).
  int cnt_size = 0;
  if (comm_type == Communicator::GLOBAL) {
    cnt_size = size_;
  } else if (comm_type == Communicator::GRAPH) {
    cnt_size = neighbor_indgree_;
  }

  int* send_count = new int[1];
  send_count[0] = entry.tensor->shape().dim_size(0);
  int* gather_count = new int[cnt_size];
  int ret_code = -1;
  if (comm_type == Communicator::GLOBAL) {
    ret_code = MPI_Allgather(send_count, 1, MPI_INT, gather_count, 1, MPI_INT,
                             mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL));
  } else if (comm_type == Communicator::GRAPH) {
    ret_code = MPI_Neighbor_allgather(
        send_count, 1, MPI_INT, gather_count, 1, MPI_INT,
        mpi_ctx_.GetMPICommunicator(Communicator::GRAPH));
  }

  if (ret_code != MPI_SUCCESS) {
    throw std::runtime_error(
        "MPI_Allgather (pre-allgather to get size) failed, see MPI output for "
        "details.");
  }

  // Copy tensor sizes from the response into a vector of int64_t
  // and compute total size.  This is size of first dimension.
  int64_t total_entry_dimension_size = 0;
  for (int rc = 0; rc < cnt_size; ++rc) {
    total_entry_dimension_size += gather_count[rc];
    recvcounts[rc] = single_slice_shape.num_elements() * gather_count[rc];
  }
  BFLOG(TRACE, rank_) << "total_entry_dimension_size: "
                    << total_entry_dimension_size;

  // Allgather output will have shape of:
  // (sum of first dimension of every tensor) x (tensor slice shape).
  TensorShape output_shape;

  output_shape.AddDim((int64_t)total_entry_dimension_size);
  output_shape.AppendShape(single_slice_shape);

  Status status = entry.context->AllocateOutput(output_shape, &entry.output);
  return status;
}

void MPIController::SetDisplacements(const int* recvcounts, int*& displcmnts,
                                     Communicator comm_type) {
  int cnt_size = 0;
  if (comm_type == Communicator::GLOBAL) {
    cnt_size = size_;
  } else if (comm_type == Communicator::GRAPH) {
    cnt_size = neighbor_indgree_;
  }
  for (int rc = 0; rc < cnt_size; ++rc) {
    if (rc == 0) {
      displcmnts[rc] = 0;
    } else {
      displcmnts[rc] = displcmnts[rc - 1] + recvcounts[rc - 1];
    }
  }
}

void MPIController::Allgather(TensorTableEntry& entry) {
  int* recvcounts = new int[size_];
  int* displcmnts = new int[size_];
  AllocateOutput(entry, recvcounts, Communicator::GLOBAL);
  SetDisplacements(recvcounts, displcmnts, Communicator::GLOBAL);

  const void* sendbuf = entry.tensor->data();
  int num_elements = entry.tensor->shape().num_elements();
  void* buffer_data = (void*)entry.output->data();

  int ret_code = MPI_Allgatherv(
      sendbuf, num_elements, mpi_ctx_.GetMPIDataType(entry.tensor), buffer_data,
      recvcounts, displcmnts, mpi_ctx_.GetMPIDataType(entry.output),
      mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL));
  if (ret_code != MPI_SUCCESS) {
    throw std::runtime_error(
        "MPI_Allgather failed, see MPI output for details.");
  }
  delete[] recvcounts;
  delete[] displcmnts;

  entry.callback(Status::OK());
}

void MPIController::Allreduce(TensorTableEntry& entry) {
  const void* sendbuf = entry.tensor->data();
  void* buffer_data = (void*)entry.output->data();
  int num_elements = entry.tensor->shape().num_elements();
  int ret_code = MPI_Allreduce(
      sendbuf, buffer_data, num_elements, mpi_ctx_.GetMPIDataType(entry.tensor),
      MPI_SUM, mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL));
  if (ret_code != MPI_SUCCESS) {
    throw std::runtime_error(
        "MPI_AllReduce failed, see MPI output for details.");
  }
  entry.callback(Status::OK());
}

void MPIController::Broadcast(TensorTableEntry& entry) {
  const int root_rank = entry.root_rank;
  // On root rank, MPI_Bcast sends data, on other ranks it receives data.
  void* data_ptr;
  if (rank_ == root_rank) {
    data_ptr = (void*)entry.tensor->data();
  } else {
    data_ptr = (void*)entry.output->data();
  }
  int num_elements = entry.tensor->shape().num_elements();
  int ret_code =
      MPI_Bcast(data_ptr, num_elements, mpi_ctx_.GetMPIDataType(entry.tensor),
                root_rank, mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL));
  if (ret_code != MPI_SUCCESS) {
    throw std::runtime_error("MPI_Bcast failed, see MPI output for details.");
  }
  entry.callback(Status::OK());
}

int MPIController::SetTopology(int indegree, const int* sources, int outdegree,
                               const int* destinations) {
  mpi_ctx_.ResetTopoSetup();
  int res_build_graph = mpi_ctx_.BuildGraphComm(indegree, sources, outdegree, destinations);
  if(res_build_graph == -1)
      return -1;
  mpi_ctx_.DestroyWinMutex();
  mpi_ctx_.SetTopoSetup();

  // Get neighbor in/out size and ranks.
  int unused_neighbor_is_weighted_ = -1;
  MPI_Dist_graph_neighbors_count(mpi_ctx_.graph_comm, &neighbor_indgree_,
                                 &neighbor_outdgree_,
                                 &unused_neighbor_is_weighted_);

  // Clear the previous neighbor_in_ranks_ is necessary because we might
  // change the topology.
  neighbor_in_ranks_.clear();
  neighbor_in_ranks_.reserve(indegree);
  for (int i = 0; i < indegree; i++) {
    neighbor_in_ranks_.push_back(sources[i]);
  }
  std::sort(neighbor_in_ranks_.begin(), neighbor_in_ranks_.end());

  neighbor_out_ranks_.clear();
  neighbor_out_ranks_.reserve(outdegree);
  for (int i = 0; i < outdegree; i++) {
    neighbor_out_ranks_.push_back(destinations[i]);
  }
  std::sort(neighbor_out_ranks_.begin(), neighbor_out_ranks_.end());
  mpi_ctx_.DisableTopoWeights(); // Topology weights are always set at SetTopologyWeights.

  mpi_ctx_.InitializeWinMutex();
  return 1;
}

int MPIController::SetTopologyWeights(int indegree, const int* sources,
                                      const float* weights) {
  // We assume when this function is called, the base topology has already
  // been set. The order of corresponding ranks of weights is always
  //  [self-rank, ...sources]
  if (!mpi_ctx_.IsTopoSetup()) {
    return -1;
  }
  neighbor_weights_[rank_] = weights[0];
  for (int i =0; i<indegree; i++) {
    neighbor_weights_[sources[i]] = weights[i+1];
  } 
  mpi_ctx_.EnableTopoWeights();
  return 1;
}

int MPIController::LoadTopology(int* indegree, int*& sources, int* outdegree,
                                int*& destinations) {
  *indegree = neighbor_in_ranks_.size();
  sources = &neighbor_in_ranks_[0];
  *outdegree = neighbor_out_ranks_.size();
  destinations = &neighbor_out_ranks_[0];
  return 1;
}

int MPIController::LoadTopologyWeights(const std::unordered_map<int, float>*& neighbor_weights) {
  if (!mpi_ctx_.IsWeighted()) {
    return 0;
  }
  neighbor_weights = &neighbor_weights_;
  return 1;
}

void MPIController::NeighborAllgather(TensorTableEntry& entry) {
  int* recvcounts = new int[neighbor_indgree_];
  int* displcmnts = new int[neighbor_indgree_];
  if (!mpi_ctx_.IsTopoSetup()) {
    throw std::runtime_error("Topology of MPI has not been set yet.");
  }
  AllocateOutput(entry, recvcounts, Communicator::GRAPH);
  SetDisplacements(recvcounts, displcmnts, Communicator::GRAPH);

  const void* sendbuf = entry.tensor->data();
  int num_elements = entry.tensor->shape().num_elements();
  void* buffer_data = (void*)entry.output->data();

  // Pitfall: mpi_neighbor_allgather do not include itself.
  int ret_code = MPI_Neighbor_allgatherv(
      sendbuf, num_elements, mpi_ctx_.GetMPIDataType(entry.tensor), buffer_data,
      recvcounts, displcmnts, mpi_ctx_.GetMPIDataType(entry.output),
      mpi_ctx_.GetMPICommunicator(Communicator::GRAPH));
  if (ret_code != MPI_SUCCESS) {
    throw std::runtime_error(
        "MPI_Neighbor_allgather failed, see MPI output for details.");
  }
  delete[] recvcounts;
  delete[] displcmnts;

  entry.callback(Status::OK());
}

void MPIController::NeighborAllreduce(TensorTableEntry& entry) {
  const void* sendbuf = entry.tensor->data();
  int num_elements = entry.tensor->shape().num_elements();

  // MPI have no neighbor_allreduce API. So we will utilize neighbor_allgather.
  // Allgather output will have shape of:
  // (sum of first dimension of every tensor) x (tensor slice shape).
  // For allreduce, the first dimension of every tensor should be the same.
  int total_entry_dimension_size =
      entry.tensor->shape().dim_size(0) * GetNeighborSize();
  TensorShape output_shape;
  output_shape.AddDim(total_entry_dimension_size);
  for (int i = 1; i < entry.tensor->shape().dims(); ++i) {
    output_shape.AddDim(entry.tensor->shape().dim_size(i));
  }
  Status status = entry.context->AllocateOutput(output_shape, &entry.output);
  void* buffer_data = (void*)entry.output->data();

  // Pitfall: Our neighbor_allreduce include itself, while
  // mpi_neighbor_allgather do not! Because for saving the communication there
  // is no need to transfer the local info again. However, for computation view,
  // including itself is more intuitive.
  int ret_code = MPI_Neighbor_allgather(
      sendbuf, num_elements, mpi_ctx_.GetMPIDataType(entry.tensor), buffer_data,
      num_elements, mpi_ctx_.GetMPIDataType(entry.output),
      mpi_ctx_.GetMPICommunicator(Communicator::GRAPH));
  if (ret_code != MPI_SUCCESS) {
    throw std::runtime_error(
        "MPI_Neighbor_allreduce(through neighbor_allgather) failed, see MPI "
        "output for details.");
  }

  entry.callback(Status::OK());
}

bool MPIController::IsMpiUnifiedModel(){
  void* data_buf = nullptr;
  int win_size = 1;
  int element_size = 1;
  MPI_Win fake_win;
  MPI_Win_create(data_buf, win_size, element_size, MPI_INFO_NULL,
                 mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL), &fake_win);
  int flag = 0;
  int* memory_model;
  MPI_Win_get_attr(fake_win, MPI_WIN_MODEL, &memory_model, &flag);
  MPI_Win_free(&fake_win);
  if (flag == 0) {
    BFLOG(WARNING) << "Failed to get MPI_WIN_MODEL attribution";
    return false;
  }
  BFLOG(DEBUG) << "Unified MPI_WIN_MODEL support is " << (*memory_model == MPI_WIN_UNIFIED);
  return *memory_model == MPI_WIN_UNIFIED;
}

Status MPIController::WinCreate(
    std::shared_ptr<Tensor> tensor,
    std::vector<std::shared_ptr<Tensor>> neighbor_tensors,
    const std::string& name, const int device) {
  // 1. Regist a Name and create a window first.
  if (!mpi_ctx_.RegisterWindowName(name)) {
    return Status::InvalidArgument(std::string("Win_create failed with ") +
                                   name);
  }
  // 2. Get the registered window manager. 
  std::shared_ptr<WindowManager> win_manager = mpi_ctx_.GetWindowByName(name);

  // A global win hold the self memory, used by win_accumulate and win_get.
  auto global_mpi_win_ptr = std::make_shared<MPI_Win>();
  void* data_buf = (void*)tensor->data();
  int element_size = mpi_ctx_.GetMPITypeSize(tensor->dtype());
  int win_size = (tensor->shape().num_elements()) * element_size;
  MPI_Win_create(data_buf, win_size, element_size, MPI_INFO_NULL,
                 mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL),
                 global_mpi_win_ptr.get());
  win_manager->SetGlobalWin(global_mpi_win_ptr);

  // Build extra buffers for win_put.
  // For example: size=4 power two ring topology
  // r\s   0    1    2    3 
  //  0    g    x         x
  //  1    x    g    x
  //  2         x    g    x
  //  3    x         x    g
  //  The following for-loop scans along columns
  //  and the self-rank determines the rows.
  //  If there is connection, the window is associated with the neighbor_tensors.
  //  Otherwise, window is associated with null pointer.
  std::shared_ptr<MPI_Win> mpi_win_ptr;
  int neighbor_tensor_index = 0;
  for (int rank = 0; rank < size_; rank++) {
    auto mpi_win_ptr = std::make_shared<MPI_Win>();
    std::shared_ptr<Tensor> t = nullptr;
    if (rank == rank_) {
      // Sender (no need to allocate the memory with it.)
      data_buf = nullptr;
      element_size = 1;
      win_size = 0;
    } else if (std::find(neighbor_in_ranks_.begin(), neighbor_in_ranks_.end(),
                         rank) != neighbor_in_ranks_.end()) {
      // Receiver
      t = neighbor_tensors[neighbor_tensor_index++];
      data_buf = (void*)t->data();
      element_size = mpi_ctx_.GetMPITypeSize(t->dtype());
      win_size = (t->shape().num_elements()) * element_size;
    } else {
      // Just participate in a collective call.
      data_buf = nullptr;
      element_size = 1;
      win_size = 0;
    }
    MPI_Win_create(data_buf, win_size, element_size, MPI_INFO_NULL,
                   mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL),
                   mpi_win_ptr.get());
    win_manager->PushBackWinAndTensor(mpi_win_ptr, t);
  }

  return Status::OK();
}

Status MPIController::WinFree(const std::string& name) {
  if (!mpi_ctx_.UnregisterWindowName(name)) {
    return Status::InvalidArgument(std::string("Win_free failed with ") + name);
  }
  return Status::OK();
}

Status MPIController::WinFreeAll() {
  if (!mpi_ctx_.UnregisterAllWindowName()) {
    return Status::InvalidArgument(std::string("Win_free_all failed."));
  }
  BFLOG(DEBUG) << "All MPI Win has been freed.";
  return Status::OK();
}

Status MPIController::WinSync(const std::string& name) {
  auto it = mpi_ctx_.named_win_map.find(name);
  if (it == mpi_ctx_.named_win_map.end()) {
    return Status::InvalidArgument(std::string("Win_free failed with ") + name);
  }

  auto win_mananger = it->second;
  for (auto rank : neighbor_in_ranks_) {
    MPI_Win_sync(*(win_mananger->GetWinByRank(rank)));
  }

  return Status::OK();
}

Status MPIController::WinFence(const std::string& name) {
  auto it = mpi_ctx_.named_win_map.find(name);
  if (it == mpi_ctx_.named_win_map.end()) {
    return Status::InvalidArgument(std::string("Win_free failed with ") + name);
  }

  std::shared_ptr<WindowManager> win_mananger = it->second;
  for (int rank = 0; rank < size_; rank++) {
    MPI_Win_fence(0, *(win_mananger->GetWinByRank(rank)));
  }

  return Status::OK();
}

void MPIController::WinPut(TensorTableEntry& entry) {
  int num_elements = entry.tensor->shape().num_elements();
  MPI_Datatype data_type = mpi_ctx_.GetMPIDataType(entry.tensor);
  auto it = mpi_ctx_.named_win_map.find(entry.tensor_name);
  if (it == mpi_ctx_.named_win_map.end()) {
    throw std::runtime_error(std::string("Cannot find ") + entry.tensor_name);
  }
  std::shared_ptr<WindowManager> win_mananger = it->second;
  MPI_Win mpi_win = *(win_mananger->GetWinByRank(rank_));

  int target_disp = 0;  // offset in win buffer
  for (auto kv : entry.dst_weights) {
    int target_rank = kv.first;
    float weight = kv.second;
    // avoid putting the tensor for itself (NOT valid).
    if (target_rank == rank_) continue;
    auto tensor = entry.tensor->data_weight(weight);
    void* sendbuf = (void*) tensor->data();
    MPI_Win_lock(MPI_LOCK_SHARED, target_rank, MPI_MODE_NOCHECK, mpi_win);
    int ret_code = MPI_Put(sendbuf, num_elements, data_type, target_rank,
                           target_disp, num_elements, data_type, mpi_win);
    if (ret_code != MPI_SUCCESS) {
      throw std::runtime_error("MPI_Put failed, see MPI output for details.");
    }
    MPI_Win_unlock(target_rank, mpi_win);
  }
  BFLOG(TRACE, rank_) << "MPI_Put for " << entry.tensor_name << " is done.";
  entry.callback(Status::OK());
}

void MPIController::WinAccumulate(TensorTableEntry& entry) {
  int num_elements = entry.tensor->shape().num_elements();
  MPI_Datatype data_type = mpi_ctx_.GetMPIDataType(entry.tensor);
  auto it = mpi_ctx_.named_win_map.find(entry.tensor_name);
  if (it == mpi_ctx_.named_win_map.end()) {
    throw std::runtime_error(std::string("Cannot find ") + entry.tensor_name);
  }
  std::shared_ptr<WindowManager> win_mananger = it->second;
  MPI_Win mpi_win = *(win_mananger->GetWinByRank(rank_));

  int target_disp = 0;  // offset in win buffer
  for (auto kv : entry.dst_weights) {
    int target_rank = kv.first;
    float weight = kv.second;
    // avoid putting the tensor for itself (NOT valid).
    if (target_rank == rank_) continue;
    auto tensor = entry.tensor->data_weight(weight);
    void* sendbuf = (void*) tensor->data();
    MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target_rank, MPI_MODE_NOCHECK, mpi_win);
    int ret_code = MPI_Accumulate(sendbuf, num_elements, data_type, target_rank,
                                  target_disp, num_elements, data_type, MPI_SUM, mpi_win);
    if (ret_code != MPI_SUCCESS) {
      throw std::runtime_error("MPI_Accumulate failed, see MPI output for details.");
    }
    MPI_Win_unlock(target_rank, mpi_win);
  }
  BFLOG(TRACE, rank_) << "MPI_Accmulate for " << entry.tensor_name << " is done.";
  entry.callback(Status::OK());
}

void MPIController::WinGet(TensorTableEntry& entry) {
  auto it = mpi_ctx_.named_win_map.find(entry.tensor_name);
  if (it == mpi_ctx_.named_win_map.end()) {
    throw std::runtime_error(std::string("Cannot find ") + entry.tensor_name +
                             std::string(" in registered win object name."));
  }
  std::shared_ptr<WindowManager> win_mananger = it->second;

  int target_disp = 0;  // offset in win buffer
  MPI_Win mpi_win = *(win_mananger->GetGlobalWin());
  for (auto kv : entry.src_weights) {
    int target_rank = kv.first;
    float weight = kv.second;  // Unused. The real weight is happened at callback.
    // avoid getting the tensor for itself.
    if (target_rank == rank_) continue;

    auto tensor = win_mananger->GetAssociateTensorByRank(target_rank);
    void* recvbuf = (void *) tensor->data();
    int num_elements = tensor->shape().num_elements();
    MPI_Datatype data_type = mpi_ctx_.GetMPIDataType(tensor);

    BFLOG(DEBUG, rank_) << "MPI_Get for " << entry.tensor_name << " is to get "
                        << num_elements << " from " << target_rank;

    MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target_rank, MPI_MODE_NOCHECK, mpi_win);
    int ret_code = MPI_Get(recvbuf, num_elements , data_type, target_rank,
                           target_disp, num_elements, data_type, mpi_win);
    if (ret_code != MPI_SUCCESS) {
      throw std::runtime_error("MPI_Get failed, see MPI output for details.");
    }
    MPI_Win_unlock(target_rank, mpi_win);
  }

  BFLOG(TRACE, rank_) << "Win_get for " << entry.tensor_name << " is done.";
  entry.callback(Status::OK());
}

void MPIController::Barrier(TensorTableEntry& entry) {
  int ret_code = MPI_Barrier(mpi_ctx_.GetMPICommunicator(Communicator::GLOBAL));
  if (ret_code != MPI_SUCCESS) {
      throw std::runtime_error("MPI_Barrier failed, see MPI output for details.");
  }
  entry.callback(Status::OK());
}

Status MPIController::WinLock(const std::string& name) {
  auto it = mpi_ctx_.named_win_map.find(name);
  if (it == mpi_ctx_.named_win_map.end()) {
    return Status::InvalidArgument(
        std::string("Cannot find ") + name +
        std::string(" in registered win object name."));
  }
  std::shared_ptr<WindowManager> win_mananger = it->second;
  MPI_Win mpi_win = *(win_mananger->GetGlobalWin());

  // It only locks the memory in local.
  int target_rank = rank_;
  MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target_rank, MPI_MODE_NOCHECK, mpi_win);

  for(const int& rank: neighbor_in_ranks_) {
    auto mpi_win_ptr = win_mananger->GetWinByRank(rank);
    MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target_rank, MPI_MODE_NOCHECK, *mpi_win_ptr);
  }

  return Status::OK();
}

Status MPIController::WinUnlock(const std::string& name){
  auto it = mpi_ctx_.named_win_map.find(name);
  if (it == mpi_ctx_.named_win_map.end()) {
    return Status::InvalidArgument(
        std::string("Cannot find ") + name +
        std::string(" in registered win object name."));
  }
  std::shared_ptr<WindowManager> win_mananger = it->second;
  MPI_Win mpi_win = *(win_mananger->GetGlobalWin());

  // It only locks the memory in local.
  int target_rank = rank_;
  MPI_Win_unlock(target_rank, mpi_win);

  for(const int& rank: neighbor_in_ranks_) {
    auto mpi_win_ptr = win_mananger->GetWinByRank(rank);
    MPI_Win_unlock(target_rank, *mpi_win_ptr);
  }

  return Status::OK();
}

Status MPIController::WinMutexAcquire() {
  if (mpi_ctx_.win_mutex.empty()) {
    return Status::PreconditionError(
        "Cannot accquire Win Mutex because it may not be initialized or has "
        "been destroyed.");
  }

  // TODO(ybc) Try better implementation than Spin Lock.
  // Recall that we build N windows across all N processes.
  // The spin value is stored in the rank i for i-th window.
  // Other process will got to acquire it.
  int one = 1;
  int minus_one = -1;
  int oldval = 0;
  int target_disp = 0;

  std::shared_ptr<MPI_Win> mutex_win;
  // Note we don't need to lock the self_win because no other process will
  // modify the memory associated with the self_win.
  for (int in_rank : neighbor_in_ranks_) {
    mutex_win = mpi_ctx_.win_mutex[in_rank];
    BFLOG(DEBUG, rank_) << "Get Win Mutex for rank " << in_rank;
    MPI_Win_lock(MPI_LOCK_SHARED, in_rank, 0, *mutex_win);
    do {
      MPI_Fetch_and_op(&one, &oldval, MPI_INT, in_rank, target_disp, MPI_SUM,
                       *mutex_win);
      MPI_Win_flush(in_rank, *mutex_win);
      if (oldval == 0) break;
      MPI_Accumulate(&minus_one, 1, MPI_INT, in_rank, target_disp, 1, MPI_INT,
                     MPI_SUM, *mutex_win);
      MPI_Win_flush(in_rank, *mutex_win);

      std::this_thread::sleep_for(std::chrono::microseconds(1));
    } while (1);
    MPI_Win_unlock(in_rank, *mutex_win);
  }

  return Status::OK();
}

Status MPIController::WinMutexRelease() {
  int minus_one = -1;
  int target_disp = 0;

  if (mpi_ctx_.win_mutex.empty()) {
    return Status::PreconditionError(
        "Cannot release Win Mutex because it may not be initialized or has "
        "been destroyed.");
  }

  std::shared_ptr<MPI_Win> mutex_win;
  for (int in_rank : neighbor_in_ranks_) {
    mutex_win = mpi_ctx_.win_mutex[in_rank];
    MPI_Win_lock(MPI_LOCK_SHARED, in_rank, 0, *mutex_win);
    // TODO(ybc) Notice the following accumulate may cause the value to be
    // negative, i.e. more release ops is called than acquire.
    MPI_Accumulate(&minus_one, 1, MPI_INT, in_rank, target_disp, 1, MPI_INT,
                   MPI_SUM, *mutex_win);
    MPI_Win_unlock(in_rank, *mutex_win);
  }

  return Status::OK();
}

}  // namespace common
}  // namespace bluefog