""" Run basic consensus algorithm.

mpirun -np 16 --oversubscribe python pytorch_convex_opt.py
"""
import torch
import numpy as np
import bluefog.torch as bf
from bluefog.common import topology_util


def bfprint(*args, **kwargs):
    if bf.rank() == 0:
        print(*args, **kwargs)


bf.init()

x = torch.Tensor([[bf.rank()]])
for i in range(100):
    x = bf.neighbor_allreduce(x)

# Expected average should be (0+1+2+...+size-1)/(size) = (size-1)/2
print("Rank {}: Normal consensus result".format(bf.rank()), x)

# Change to star topology with hasting rule, which should be unbiased as well.
bf.set_topology(topology_util.StarGraph(bf.size()), is_weighted=True)
x = torch.Tensor([[bf.rank()]])
for i in range(100):
    x = bf.neighbor_allreduce(x)

# Expected average should be (0+1+2+...+size-1)/(size) = (size-1)/2
print("Rank {}: consensus with weights".format(bf.rank()), x)

# Use win_accumulate to simulate the push-sum algorithm (sync).
bf.set_topology(topology_util.PowerTwoRingGraph(bf.size()))
outdegree = len(bf.out_neighbor_ranks())
indegree = len(bf.in_neighbor_ranks())

# Remember we do not create buffer with 0.
p = torch.Tensor([[1.0/bf.size()/(indegree+1)]])
bf.win_create(p, name="p_buff")
p = bf.win_sync_then_collect(name="p_buff")


x = torch.Tensor([[bf.rank()/(indegree+1)]])
bf.win_create(x, name="x_buff")
x = bf.win_sync_then_collect(name="x_buff")

for i in range(200):
    handle1 = bf.win_accumulate(p, name="p_buff", dst_weights={
        rank: 1.0 / (outdegree + 1) for rank in bf.out_neighbor_ranks()})
    handle2 = bf.win_accumulate(x, name="x_buff", dst_weights={
        rank: 1.0 / (outdegree + 1) for rank in bf.out_neighbor_ranks()})
    bf.win_wait(handle1)
    bf.win_wait(handle2)
    x.mul_(1.0/(1+outdegree))
    p.mul_(1.0/(1+outdegree))  # Do not forget to update self!

    bf.barrier()
    with bf.win_lock("p_buff"), bf.win_lock("x_buff"):
        p = bf.win_sync_then_collect(name="p_buff")
        x = bf.win_sync_then_collect(name="x_buff")


bf.barrier()
print("Rank {}: consensus with win ops p: {}, x: {}, x/p: {}".format(bf.rank(), p, x, x/p))

sum_push_sum = bf.allreduce(x/p, average=False)
if bf.rank() == 0:
    print("Total Sum ", sum_push_sum)

bf.win_free(name="x_buff")
bf.win_free(name="p_buff")