/* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 * Channel-ID specialization of libc++19 sort.h (llvmorg-19.1.0), verified
 * against the pinned client's 0x7fd464. See LLVM-LICENSE.txt in this directory.
 * At most 32 elements: ninther, bitset and heap fallback cannot be reached. */
#include "outgoing.h"
#include <assert.h>
static bool before(const IHS_Outgoing *o, uint8_t a, uint8_t b) {
    return o->priority[a] > o->priority[b];
}
static void swap(uint8_t *a, uint8_t *b) {
    uint8_t t = *a;
    *a = *b;
    *b = t;
}
static void sort3(IHS_Outgoing *o, int x, int y, int z) {
    uint8_t *a = o->order;
    if (!before(o, a[y], a[x])) {
        if (!before(o, a[z], a[y]))
            return;
        swap(a + y, a + z);
        if (before(o, a[y], a[x]))
            swap(a + x, a + y);
    } else if (before(o, a[z], a[y]))
        swap(a + x, a + z);
    else {
        swap(a + x, a + y);
        if (before(o, a[z], a[y]))
            swap(a + y, a + z);
    }
}
static bool insertion(IHS_Outgoing *o, int lo, int hi, bool incomplete) {
    uint8_t *a = o->order;
    int start = lo + 1;
    if (incomplete && hi - lo > 5) {
        sort3(o, lo, lo + 1, lo + 2);
        start = lo + 3;
    }
    unsigned moved = 0;
    for (int i = start; i < hi; ++i) {
        uint8_t value = a[i];
        int j = i;
        while (j > lo && before(o, value, a[j - 1])) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = value;
        if (incomplete && hi - lo > 5 && j != i && ++moved == 8)
            return i + 1 == hi;
    }
    return true;
}
static int equal_left(IHS_Outgoing *o, int lo, int hi) {
    uint8_t *a = o->order, pivot = a[lo];
    int first = lo, last = hi;
    if (before(o, pivot, a[last - 1])) {
        do {
            ++first;
        } while (!before(o, pivot, a[first]));
    } else
        while (++first < last && !before(o, pivot, a[first])) {
        }
    if (first < last)
        do {
            --last;
        } while (before(o, pivot, a[last]));
    while (first < last) {
        swap(a + first, a + last);
        do {
            ++first;
        } while (!before(o, pivot, a[first]));
        do {
            --last;
        } while (before(o, pivot, a[last]));
    }
    int pos = first - 1;
    if (pos != lo)
        a[lo] = a[pos];
    a[pos] = pivot;
    return first;
}
static int equal_right(IHS_Outgoing *o, int lo, int hi, bool *partitioned) {
    uint8_t *a = o->order, pivot = a[lo];
    int first = lo, last = hi;
    do {
        ++first;
    } while (before(o, a[first], pivot));
    if (first == lo + 1) {
        while (first < last && !before(o, a[--last], pivot)) {
        }
    } else
        do {
            --last;
        } while (!before(o, a[last], pivot));
    *partitioned = first >= last;
    while (first < last) {
        swap(a + first, a + last);
        do {
            ++first;
        } while (before(o, a[first], pivot));
        do {
            --last;
        } while (!before(o, a[last], pivot));
    }
    int pos = first - 1;
    if (pos != lo)
        a[lo] = a[pos];
    a[pos] = pivot;
    return pos;
}
static void sort_range(IHS_Outgoing *o, int lo, int hi, bool leftmost) {
    while (hi - lo >= 24) {
        sort3(o, lo + (hi - lo) / 2, lo, hi - 1);
        if (!leftmost && !before(o, o->order[lo - 1], o->order[lo])) {
            lo = equal_left(o, lo, hi);
            continue;
        }
        bool partitioned;
        int pivot = equal_right(o, lo, hi, &partitioned);
        if (partitioned) {
            bool left = insertion(o, lo, pivot, true);
            if (insertion(o, pivot + 1, hi, true)) {
                if (left)
                    return;
                hi = pivot;
                continue;
            } else if (left) {
                lo = pivot + 1;
                continue;
            }
        }
        sort_range(o, lo, pivot, leftmost);
        lo = pivot + 1;
        leftmost = false;
    }
    insertion(o, lo, hi, false);
}
void IHS_OutgoingSort(IHS_Outgoing *o) {
    assert(o->count <= 32);
    sort_range(o, 0, o->count, true);
}
