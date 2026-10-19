// VITA_SORT_WALL_EDGES_BEGIN (tools/vita/sortwalledges_test.py cuts this out)
/*
============
Vita_MergeWallEdgeSequences

SortWallEdges' merge loop - the same merges, in the same order, with the same
result - without its restart. The stock loop starts over from the first
sequence after every merge: with the 256 wall edges an obstacle search can
gather, that is millions of comparisons (3.7 ms a frame, 15 ms at worst, in
the benchmark's Alpha Labs views with soldiers walking around).

Why the restart can go: the stock loop only ever changes a sequence's first
edge (when it prepends another sequence) - never a last edge, not even when it
appends, which leaves sequenceLast stale; kept as is. So when the scan reaches
sequence i, no pair among the earlier ones matches, and after a merge at i the
only pairs that can have changed are an earlier sequence k against i, and only
when i got a new first edge: "k's last meets i's first". The restart's scan
would then stop at the first such k (all of k's other pairs still fail) and
append i to it. Otherwise it would find nothing before i and rescan i. Both
are done here directly. tools/vita/sortwalledges_test.py checks it against
the stock loop on random edge sets, every output entry.
============
*/
static int Vita_MergeWallEdgeSequences(wallEdge_t** sequenceFirst, wallEdge_t** sequenceLast, int numSequences)
{
    int i = 0;
    while (i < numSequences) {
        int j;
        bool prepended = false;
        for (j = i + 1; j < numSequences; j++) {
            if (sequenceFirst[i]->verts[0] == sequenceLast[j]->verts[1]) {
                sequenceLast[j]->next = sequenceFirst[i];
                sequenceFirst[i] = sequenceFirst[j];
                prepended = true;
                break;
            }
            if (sequenceLast[i]->verts[1] == sequenceFirst[j]->verts[0]) {
                sequenceLast[i]->next = sequenceFirst[j];
                break;
            }
        }
        if (j == numSequences) {
            i++;
            continue;
        }
        numSequences--;
        for (int k = j; k < numSequences; k++) {
            sequenceFirst[k] = sequenceFirst[k + 1];
            sequenceLast[k] = sequenceLast[k + 1];
        }
        if (!prepended) {
            continue; // the stock restart finds nothing before i, then scans i again
        }
        // i starts somewhere new: the first earlier sequence ending there takes it
        int k;
        for (k = 0; k < i; k++) {
            if (sequenceLast[k]->verts[1] == sequenceFirst[i]->verts[0]) {
                break;
            }
        }
        if (k < i) {
            sequenceLast[k]->next = sequenceFirst[i];
            numSequences--;
            for (int m = i; m < numSequences; m++) {
                sequenceFirst[m] = sequenceFirst[m + 1];
                sequenceLast[m] = sequenceLast[m + 1];
            }
        }
    }
    return numSequences;
}
