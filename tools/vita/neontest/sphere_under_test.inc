static float R_SegmentToSegmentDistanceSquare(const idVec3& start1, const idVec3& end1, const idVec3& start2, const idVec3& end2)
{

    const idVec3 dir0 = start1 - start2;
    const idVec3 dir1 = end1 - start1;
    const idVec3 dir2 = end2 - start2;

    const float dotDir1Dir1 = dir1 * dir1;
    const float dotDir2Dir2 = dir2 * dir2;
    const float dotDir1Dir2 = dir1 * dir2;
    const float dotDir0Dir1 = dir0 * dir1;
    const float dotDir0Dir2 = dir0 * dir2;

    if (dotDir1Dir1 < idMath::FLT_SMALLEST_NON_DENORMAL || dotDir2Dir2 < idMath::FLT_SMALLEST_NON_DENORMAL) {
        // At least one of the lines is degenerate.
        // The returned length is correct only if both lines are degenerate otherwise the start point of
        // the degenerate line has to be projected onto the other line to calculate the shortest distance.
        // The degenerate case is not relevant here though.
        return (start2 - start1).LengthSqr();
    }

    const float d = dotDir1Dir1 * dotDir2Dir2 - dotDir1Dir2 * dotDir1Dir2;

    if (d < idMath::FLT_SMALLEST_NON_DENORMAL) {
        // The lines are parallel.
        // The returned length is not correct.
        // The parallel case is not relevent here though.
        return (start2 - start1).LengthSqr();
    }

    const float n = dotDir0Dir2 * dotDir1Dir2 - dotDir0Dir1 * dotDir2Dir2;

    const float t1 = n / d;
    const float t1c = idMath::ClampFloat(0.0f, 1.0f, t1);

    const float t2 = (dotDir0Dir2 + dotDir1Dir2 * t1) / dotDir2Dir2;
    const float t2c = idMath::ClampFloat(0.0f, 1.0f, t2);

    const idVec3 closest1 = start1 + (dir1 * t1c);
    const idVec3 closest2 = start2 + (dir2 * t2c);

    const float distSqr = (closest2 - closest1).LengthSqr();

    return distSqr;
}

/*
===================
R_LineIntersectsTriangleExpandedWithSphere
===================
*/
bool R_LineIntersectsTriangleExpandedWithSphere(const idVec3& lineStart, const idVec3& lineEnd, const idVec3& lineDir, const float lineLength,
    const float sphereRadius, const idVec3& triVert0, const idVec3& triVert1, const idVec3& triVert2)
{
    // edge directions
    const idVec3 edge1 = triVert1 - triVert0;
    const idVec3 edge2 = triVert2 - triVert0;

    // calculate determinant
    const idVec3 tvec = lineStart - triVert0;
    const idVec3 pvec = lineDir.Cross(edge1);
    const idVec3 qvec = tvec.Cross(edge2);
    const float det = edge2 * pvec;

    // calculate UV parameters
    const float u = (tvec * pvec) * det;
    const float v = (lineDir * qvec) * det;

    // test if the line passes through the triangle
    if (u >= 0.0f && u <= det * det) {
        if (v >= 0.0f && u + v <= det * det) {
            // if determinant is near zero then the ray lies in the triangle plane
            if (idMath::Fabs(det) > idMath::FLT_SMALLEST_NON_DENORMAL) {
                const float fraction = (edge1 * qvec) / det;
                if (fraction >= 0.0f && fraction <= lineLength) {
                    return true;
                }
            }
        }
    }

    const float radiusSqr = sphereRadius * sphereRadius;

    if (R_SegmentToSegmentDistanceSquare(lineStart, lineEnd, triVert0, triVert1) < radiusSqr) {
        return true;
    }

    if (R_SegmentToSegmentDistanceSquare(lineStart, lineEnd, triVert1, triVert2) < radiusSqr) {
        return true;
    }

    if (R_SegmentToSegmentDistanceSquare(lineStart, lineEnd, triVert2, triVert0) < radiusSqr) {
        return true;
    }

    return false;
}

