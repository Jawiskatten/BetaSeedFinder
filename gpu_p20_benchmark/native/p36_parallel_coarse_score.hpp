#pragma once

#include "coarse_exact_gpu.hpp"

namespace p36score {

using namespace coarsegpu;

__device__ __forceinline__ void tryEnqueueCoarse(
        const unsigned char* signs,
        int* labels,
        int* queue,
        int componentId,
        int neighborIndex,
        int* sharedTail
) {
    if (signs[neighborIndex] == 0) return;
    if (atomicCAS(labels + neighborIndex, 0, componentId) == 0) {
        const int slot = atomicAdd(sharedTail, 1);
        queue[slot] = neighborIndex;
    }
}

// Exact block-parallel equivalent of coarsegpu::scoreCoarseSignsKernel.
//
// The production scorer assigns one GPU thread to each 61x61x17 sign grid.
// This probe preserves the same linear component-start order, six-neighbour
// connectivity, boundary tests, and re-entry rule, but lets a block process
// each BFS frontier cooperatively. Components are still completed one at a
// time, so the component IDs and all score decisions remain deterministic.
template<int BLOCK_THREADS>
__global__ void scoreCoarseSignsParallelKernel(
        const unsigned char* signs,
        int count,
        int* labels,
        int* queue,
        int* columnSeen,
        int* columnMinY,
        int* componentColumns,
        int* scores
) {
    static_assert(BLOCK_THREADS == 32 || BLOCK_THREADS == 64 || BLOCK_THREADS == 128,
            "P36 benchmark supports 32, 64, or 128 threads");

    const int seedIndex = blockIdx.x;
    const int lane = threadIdx.x;
    if (seedIndex >= count) return;

    const std::size_t cellBase = static_cast<std::size_t>(seedIndex) * coarsecore::CELLS;
    const std::size_t colBase = static_cast<std::size_t>(seedIndex) * coarsecore::COLUMNS;
    int* seedLabels = labels + cellBase;
    int* seedQueue = queue + cellBase;
    int* seedColumnSeen = columnSeen + colBase;
    int* seedColumnMinY = columnMinY + colBase;
    int* seedComponentColumns = componentColumns + colBase;
    const unsigned char* seedSigns = signs + cellBase;

    for (int i = lane; i < coarsecore::CELLS; i += BLOCK_THREADS) seedLabels[i] = 0;
    for (int i = lane; i < coarsecore::COLUMNS; i += BLOCK_THREADS) {
        seedColumnSeen[i] = 0;
        seedColumnMinY[i] = coarsecore::Y_LEVELS;
    }
    __syncthreads();

    __shared__ int searchCursor;
    __shared__ int startCell;
    __shared__ int componentId;
    __shared__ int nextId;
    __shared__ int best;
    __shared__ int head;
    __shared__ int tail;
    __shared__ int frontierEnd;
    __shared__ int cells;
    __shared__ int maxY;
    __shared__ int flags;
    __shared__ int columnCount;
    __shared__ int candidate;
    __shared__ int reentry;

    if (lane == 0) {
        searchCursor = 0;
        nextId = 1;
        best = 0;
    }
    __syncthreads();

    while (true) {
        if (lane == 0) {
            int found = -1;
            for (int i = searchCursor; i < coarsecore::CELLS; ++i) {
                if (seedLabels[i] == 0 && seedSigns[i] != 0) {
                    found = i;
                    searchCursor = i + 1;
                    break;
                }
            }
            startCell = found;
            if (found >= 0) {
                componentId = nextId++;
                head = 0;
                tail = 1;
                seedQueue[0] = found;
                seedLabels[found] = componentId;
                cells = 0;
                maxY = 0;
                flags = 0; // bit 0: bottom, bit 1: side
                columnCount = 0;
            }
        }
        __syncthreads();
        if (startCell < 0) break;

        while (true) {
            if (lane == 0) frontierEnd = tail;
            __syncthreads();

            const int begin = head;
            const int end = frontierEnd;
            int localCells = 0;
            int localMaxY = 0;
            int localFlags = 0;

            for (int pos = begin + lane; pos < end; pos += BLOCK_THREADS) {
                const int idx = seedQueue[pos];
                const int cy = idx % coarsecore::Y_LEVELS;
                const int tmp = idx / coarsecore::Y_LEVELS;
                const int cz = tmp % coarsecore::SIZE;
                const int cx = tmp / coarsecore::SIZE;

                ++localCells;
                if (cy > localMaxY) localMaxY = cy;
                if (cy == 0) localFlags |= 1;
                if (cx == 0 || cz == 0 || cx == coarsecore::SIZE - 1 || cz == coarsecore::SIZE - 1) {
                    localFlags |= 2;
                }

                const int col = cx * coarsecore::SIZE + cz;
                if (atomicCAS(seedColumnSeen + col, 0, componentId) == 0) {
                    const int slot = atomicAdd(&columnCount, 1);
                    seedComponentColumns[slot] = col;
                }
                atomicMin(seedColumnMinY + col, cy);

                if (cx + 1 < coarsecore::SIZE) {
                    tryEnqueueCoarse(seedSigns, seedLabels, seedQueue, componentId,
                            coarsecore::index3(cx + 1, cy, cz), &tail);
                }
                if (cx > 0) {
                    tryEnqueueCoarse(seedSigns, seedLabels, seedQueue, componentId,
                            coarsecore::index3(cx - 1, cy, cz), &tail);
                }
                if (cy + 1 < coarsecore::Y_LEVELS) {
                    tryEnqueueCoarse(seedSigns, seedLabels, seedQueue, componentId,
                            coarsecore::index3(cx, cy + 1, cz), &tail);
                }
                if (cy > 0) {
                    tryEnqueueCoarse(seedSigns, seedLabels, seedQueue, componentId,
                            coarsecore::index3(cx, cy - 1, cz), &tail);
                }
                if (cz + 1 < coarsecore::SIZE) {
                    tryEnqueueCoarse(seedSigns, seedLabels, seedQueue, componentId,
                            coarsecore::index3(cx, cy, cz + 1), &tail);
                }
                if (cz > 0) {
                    tryEnqueueCoarse(seedSigns, seedLabels, seedQueue, componentId,
                            coarsecore::index3(cx, cy, cz - 1), &tail);
                }
            }

            if (localCells != 0) {
                atomicAdd(&cells, localCells);
                atomicMax(&maxY, localMaxY);
                if (localFlags != 0) atomicOr(&flags, localFlags);
            }
            __syncthreads();

            if (lane == 0) head = frontierEnd;
            __syncthreads();
            if (head >= tail) break;
        }

        if (lane == 0) {
            candidate = cells > best
                    && maxY >= coarsecore::MIN_INTERESTING_Y
                    && (flags & 1) == 0
                    && (flags & 2) == 0;
            reentry = 0;
        }
        __syncthreads();

        if (candidate != 0) {
            for (int i = lane; i < columnCount; i += BLOCK_THREADS) {
                const int col = seedComponentColumns[i];
                const int cx = col / coarsecore::SIZE;
                const int cz = col - cx * coarsecore::SIZE;
                const int minY = seedColumnMinY[col];
                if (minY <= 0 || minY >= coarsecore::Y_LEVELS) continue;
                for (int lowerY = minY - 1; lowerY >= 0; --lowerY) {
                    const int idx = coarsecore::index3(cx, lowerY, cz);
                    if (seedSigns[idx] != 0 && seedLabels[idx] != componentId) {
                        if (minY - lowerY - 1 >= 1) atomicExch(&reentry, 1);
                        break;
                    }
                }
            }
        }
        __syncthreads();

        if (lane == 0 && reentry != 0) best = cells;
        __syncthreads();

        // Reuse the per-column scratch without carrying component IDs across
        // components. Resetting only touched columns is exact and avoids a full
        // 3,721-column clear for every component.
        for (int i = lane; i < columnCount; i += BLOCK_THREADS) {
            const int col = seedComponentColumns[i];
            seedColumnSeen[col] = 0;
            seedColumnMinY[col] = coarsecore::Y_LEVELS;
        }
        __syncthreads();
    }

    if (lane == 0) scores[seedIndex] = best;
}

} // namespace p36score
