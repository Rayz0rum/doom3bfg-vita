// idAASLocal::SortWallEdges: the restart-free version (AAS_pathing.cpp, Vita)
// against id's original loop, on random edge sets - every entry of the output
// must match, including the ones the original's quirk leaves stale (it never
// moves a sequence's last edge when it appends, so a later append can cut a
// chain off).
//
//   python tools/vita/sortwalledges_test.py   (cuts the engine's block, builds, runs)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct wallEdge_t {
    int edgeNum;
    int verts[2];
    wallEdge_t* next;
};

static std::vector<int> v0s, v1s; // edge number -> vertex numbers

static void GetEdgeVertexNumbers(int edgeNum, int verts[2])
{
    if (edgeNum < 0) {
        verts[0] = v1s[-edgeNum];
        verts[1] = v0s[-edgeNum];
    } else {
        verts[0] = v0s[edgeNum];
        verts[1] = v1s[edgeNum];
    }
}

static void Original(int* edges, int numEdges)
{
    int i, j, k, numSequences;
    std::vector<wallEdge_t> wallEdgesV(numEdges);
    std::vector<wallEdge_t*> firstV(numEdges), lastV(numEdges);
    wallEdge_t **sequenceFirst = firstV.data(), **sequenceLast = lastV.data(), *wallEdges = wallEdgesV.data(), *wallEdge;
    for (i = 0; i < numEdges; i++) {
        wallEdges[i].edgeNum = edges[i];
        GetEdgeVertexNumbers(edges[i], wallEdges[i].verts);
        wallEdges[i].next = NULL;
        sequenceFirst[i] = &wallEdges[i];
        sequenceLast[i] = &wallEdges[i];
    }
    numSequences = numEdges;
    for (i = 0; i < numSequences; i++) {
        for (j = i + 1; j < numSequences; j++) {
            if (sequenceFirst[i]->verts[0] == sequenceLast[j]->verts[1]) {
                sequenceLast[j]->next = sequenceFirst[i];
                sequenceFirst[i] = sequenceFirst[j];
                break;
            }
            if (sequenceLast[i]->verts[1] == sequenceFirst[j]->verts[0]) {
                sequenceLast[i]->next = sequenceFirst[j];
                break;
            }
        }
        if (j < numSequences) {
            numSequences--;
            for (k = j; k < numSequences; k++) {
                sequenceFirst[k] = sequenceFirst[k + 1];
                sequenceLast[k] = sequenceLast[k + 1];
            }
            i = -1;
        }
    }
    k = 0;
    for (i = 0; i < numSequences; i++) {
        for (wallEdge = sequenceFirst[i]; wallEdge; wallEdge = wallEdge->next) {
            edges[k++] = wallEdge->edgeNum;
        }
    }
}

// cut from neo/d3xp/ai/AAS_pathing.cpp by the build line in the header comment
#include "sortwalledges_under_test.inc"

static void Vita_SortWallEdges(int* edges, int numEdges)
{
    std::vector<wallEdge_t> wallEdgesV(numEdges);
    std::vector<wallEdge_t*> firstV(numEdges), lastV(numEdges);
    for (int i = 0; i < numEdges; i++) {
        wallEdgesV[i].edgeNum = edges[i];
        GetEdgeVertexNumbers(edges[i], wallEdgesV[i].verts);
        wallEdgesV[i].next = NULL;
        firstV[i] = &wallEdgesV[i];
        lastV[i] = &wallEdgesV[i];
    }
    const int numSequences = Vita_MergeWallEdgeSequences(firstV.data(), lastV.data(), numEdges);
    int k = 0;
    for (int i = 0; i < numSequences; i++) {
        for (wallEdge_t* wallEdge = firstV[i]; wallEdge; wallEdge = wallEdge->next) {
            edges[k++] = wallEdge->edgeNum;
        }
    }
}

int main()
{
    srand(1234);
    long long trials = 0, entries = 0, stale = 0;
    int bad = 0;
    for (int t = 0; t < 200000 && bad < 5; t++) {
        const int numVerts = 2 + rand() % 40;
        const int numEdges = 1 + rand() % 256;
        v0s.assign(numEdges + 1, 0);
        v1s.assign(numEdges + 1, 0);
        std::vector<int> edges(numEdges);
        for (int e = 1; e <= numEdges; e++) {
            if (rand() % 3) {
                // chains: e continues from e-1 most of the time
                v0s[e] = (e > 1 && rand() % 4) ? v1s[e - 1] : rand() % numVerts;
            } else {
                v0s[e] = rand() % numVerts;
            }
            v1s[e] = rand() % numVerts;
        }
        // a random order, random signs
        for (int e = 0; e < numEdges; e++) {
            edges[e] = (e + 1) * ((rand() & 1) ? 1 : -1);
        }
        for (int e = numEdges - 1; e > 0; e--) {
            const int s = rand() % (e + 1);
            const int tmp = edges[e];
            edges[e] = edges[s];
            edges[s] = tmp;
        }
        std::vector<int> a = edges, b = edges;
        Original(a.data(), numEdges);
        Vita_SortWallEdges(b.data(), numEdges);
        trials++;
        entries += numEdges;
        if (a != b) {
            printf("MISMATCH in trial %d (%d edges)\n", t, numEdges);
            bad++;
        }
        // how often the quirk bites (an edge missing from the chains)
        std::vector<int> seen(numEdges + 1, 0);
        for (int e = 0; e < numEdges; e++) {
            seen[abs(a[e])]++;
        }
        for (int e = 1; e <= numEdges; e++) {
            stale += (seen[e] != 1);
        }
    }
    printf("sort wall edges: %lld trials, %lld entries, %d mismatches (the original's stale-tail quirk touched %lld entries)\n", trials, entries, bad, stale);
    return bad ? 1 : 0;
}
