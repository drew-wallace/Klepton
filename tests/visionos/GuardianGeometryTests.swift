// Run: swiftc visionos/Sources/KleptonGuardianGeometry.swift \
//      tests/visionos/GuardianGeometryTests.swift -o /tmp/guardian-tests && /tmp/guardian-tests
import Foundation

@main struct GuardianGeometryTests {
    static func main() {
        typealias G = KLGuardianGeometry
        let square: [G.Point] = [SIMD2(-2, -2), SIMD2(2, -2), SIMD2(2, 2), SIMD2(-2, 2)]
        precondition(G.validPolygon(square))
        precondition(G.contains(.zero, polygon: square))
        precondition(!G.contains(SIMD2(3, 0), polygon: square))
        let concave: [G.Point] = [SIMD2(0, 0), SIMD2(3, 0), SIMD2(3, 1), SIMD2(1, 1), SIMD2(1, 3), SIMD2(0, 3)]
        precondition(G.validPolygon(concave))
        precondition(G.contains(SIMD2(0.5, 2), polygon: concave))
        precondition(!G.contains(SIMD2(2, 2), polygon: concave))
        precondition(!G.validPolygon([square[0], square[2], square[1], square[3]]))
        precondition(!G.validPolygon([.zero, SIMD2(0.2, 0), SIMD2(0, 0.2)]))
        precondition(!G.validPolygon([.zero, SIMD2(Float.nan, 1), SIMD2(1, 0)]))
        precondition(G.reveal(clearance: 1) == 0)
        precondition(G.reveal(clearance: 0.05) == 1)
        precondition(G.reveal(clearance: -1) == 1)
        precondition(G.reveal(clearance: .nan) == 1)
        var last: Float = 0
        for i in 0...100 {
            let reveal = G.reveal(clearance: 0.3 - Float(i) * 0.003)
            precondition(reveal >= last && reveal <= 1); last = reveal
        }
        // The outline must retain internal obstacle holes and disconnected areas.
        var cells = Set((0..<9).flatMap { x in (0..<9).map { G.Cell(x: x, z: $0) } })
        cells.remove(G.Cell(x: 4, z: 4))
        cells.insert(G.Cell(x: 30, z: 30))
        let connected = G.connected(cells, from: G.Cell(x: 0, z: 0))
        precondition(connected.count == 80)
        precondition(!connected.contains(G.Cell(x: 30, z: 30)))
        let hole = G.center(G.Cell(x: 4, z: 4))
        let clearance = G.outline(connected).map { G.distance(hole, to: $0) }.min()!
        precondition(abs(clearance - G.cellSize / 2) < 0.001)
        precondition(!G.contains(hole, edges: G.outline(connected)))
        precondition(G.coveredCells(G.outline(connected)) == connected)
        let angle: Float = 0.6
        func rotate(_ p: G.Point) -> G.Point { SIMD2(cos(angle) * p.x - sin(angle) * p.y, sin(angle) * p.x + cos(angle) * p.y) }
        let rotated = G.outline(connected).map { G.Segment(a: rotate($0.a), b: rotate($0.b)) }
        precondition(!G.contains(rotate(hole), edges: rotated), "Anchor rotation must retain obstacle holes")
        precondition(G.contains(rotate(G.center(G.Cell(x: 1, z: 1))), edges: rotated))
        // A floor square with a narrow wall verifies surface interpolation
        // and a contour that stays close to the object.
        let floor: [G.Triangle] = [
            .init(a: SIMD3(0, 0, 0), b: SIMD3(3, 0, 0), c: SIMD3(3, 0, 3)),
            .init(a: SIMD3(0, 0, 0), b: SIMD3(3, 0, 3), c: SIMD3(0, 0, 3))]
        let wall: [G.Triangle] = [
            .init(a: SIMD3(1.3, 0, 0), b: SIMD3(1.3, 1, 0), c: SIMD3(1.3, 1, 3))]
        let map = G.mappedCells(floors: floor, obstacles: wall)!
        precondition(!map.cells.contains(G.cell(at: SIMD2(1.32, 1.5))), "A node inside the wall margin must be excluded")
        precondition(map.cells.contains(G.cell(at: SIMD2(1.2, 1.5))), "The area can now approach within 10 cm of an object")
        precondition(map.cells.contains(G.cell(at: SIMD2(1.4, 1.5))), "Do not retain the previous 30 cm padding")
        precondition(map.cells.contains(G.Cell(x: 0, z: 10)), "Do not add an extra cell of floor-edge erosion")
        let left = G.connected(map.cells, from: G.cell(at: SIMD2(0.5, 1.5)))
        let contour = G.contour(left, field: map.field)
        let wallEdges = contour.filter { abs($0.a.x - 1.25) < 0.015 && abs($0.b.x - 1.25) < 0.015 }
        precondition(wallEdges.contains { G.length($0.b - $0.a) > 2 }, "A mapped wall must become a long straight contour, not grid boxes")
        precondition(!G.contains(SIMD2(1.3, 1.5), edges: contour))
        precondition(G.contains(SIMD2(1.2, 1.5), edges: contour))
        let angledWall = [G.Triangle(a: SIMD3(0, 0, 0.7), b: SIMD3(0, 1, 0.7), c: SIMD3(3, 1, 2.2))]
        let angled = G.mappedCells(floors: floor, obstacles: angledWall)!
        let angledArea = G.connected(angled.cells, from: G.cell(at: SIMD2(1.5, 0.3)))
        let angledContour = G.contour(angledArea, field: angled.field)
        precondition(angledContour.contains { G.length($0.b - $0.a) > 2 && abs(($0.b.y - $0.a.y) / ($0.b.x - $0.a.x) - 0.5) < 0.02 }, "Diagonal walls must retain their physical angle")
        for i in 1..<29 {
            let x = Float(i) * 0.1, z = 0.7 + x * 0.5
            precondition(!G.contains(SIMD2(x, z), edges: angledContour), "A smooth contour must not include the physical wall")
        }
        let furniture = [
            G.Triangle(a: SIMD3(1, 0.8, 1), b: SIMD3(2, 0.8, 1), c: SIMD3(2, 0.8, 2)),
            G.Triangle(a: SIMD3(1, 0.8, 1), b: SIMD3(2, 0.8, 2), c: SIMD3(1, 0.8, 2))]
        let furnished = G.mappedCells(floors: floor, obstacles: furniture)!
        let aroundTable = G.connected(furnished.cells, from: G.cell(at: SIMD2(0.5, 0.5)))
        let furnitureContour = G.contour(aroundTable, field: furnished.field)
        precondition(!G.contains(SIMD2(1.5, 1.5), edges: furnitureContour), "Interpolated furniture contours must retain interior holes")
        precondition(G.contains(SIMD2(0.5, 0.5), edges: furnitureContour))
        let tableClearance = furnitureContour.map { G.distance(SIMD2(1.5, 1), to: $0) }.min()!
        precondition(abs(tableClearance - 0.05) < 0.012, "The contour must follow the object's actual 5 cm offset")
        let spots = G.holeLoops(furnitureContour)
        precondition(spots.count == 1 && G.contains(SIMD2(1.5, 1.5), polygon: spots[0]))
        precondition(G.hole(at: SIMD2(1.5, 1.5), in: spots) != nil)
        precondition(G.hole(at: SIMD2(0, 1.5), in: spots) == nil, "The outer room boundary must never be selectable as an unwanted spot")
        let cleared = G.removing(spots[0], from: furnitureContour)
        precondition(G.holeLoops(cleared).isEmpty && G.contains(SIMD2(1.5, 1.5), edges: cleared))
        precondition(!G.contains(SIMD2(-0.1, 1.5), edges: cleared), "Deleting a hole must preserve the outside boundary")
        let reversed = furnitureContour.reversed().map { G.Segment(a: $0.b, b: $0.a) }
        precondition(G.holeLoops(reversed).count == 1, "Hole selection must not depend on edge order or winding")
        let turnedFurniture = furnitureContour.map { G.Segment(a: rotate($0.a), b: rotate($0.b)) }
        precondition(G.hole(at: rotate(SIMD2(1.5, 1.5)), in: G.holeLoops(turnedFurniture)) != nil)
        let firstHole: [G.Point] = [SIMD2(-1.5, -1.5), SIMD2(-1, -1.5), SIMD2(-1, -1), SIMD2(-1.5, -1)]
        let secondHole = firstHole.map { $0 + SIMD2<Float>(2, 2) }
        let twoHoles = G.segments(square) + G.segments(firstHole) + G.segments(secondHole)
        let selected = G.hole(at: SIMD2(-1.25, -1.25), in: G.holeLoops(twoHoles))!
        let oneDeleted = G.removing(selected, from: twoHoles)
        precondition(G.holeLoops(oneDeleted).count == 1 && G.contains(SIMD2(-1.25, -1.25), edges: oneDeleted))
        precondition(!G.contains(SIMD2(0.75, 0.75), edges: oneDeleted), "Only the selected spot may be deleted")
        // A small densely triangulated raised patch used to outvote the room
        // floor's two large triangles, leaving only the patch around the wearer.
        var densePatch: [G.Triangle] = []
        for x in 0..<12 { for z in 0..<12 {
            let a = SIMD3<Float>(1 + Float(x) * 0.05, 0.2, 1 + Float(z) * 0.05)
            densePatch += [.init(a: a, b: a + SIMD3(0.05, 0, 0), c: a + SIMD3(0.05, 0, 0.05)),
                           .init(a: a, b: a + SIMD3(0.05, 0, 0.05), c: a + SIMD3(0, 0, 0.05))]
        } }
        let densityMap = G.mappedCells(floors: floor + densePatch, obstacles: [])!
        precondition(abs(densityMap.floor) < 0.001, "Choose floor height by physical area, not triangle count")
        precondition(Float(densityMap.cells.count) * G.cellSize * G.cellSize > 8.9,
                     "A dense local patch must not reduce a nine square metre room to a small rectangle")
        let measuredFloor = G.mappedCells(floors: floor + densePatch, obstacles: [], detectedFloor: 0)!
        precondition(measuredFloor.cells == densityMap.cells)
        let lowerPatch = densePatch.map { G.Triangle(a: $0.a - SIMD3(0, 0.2, 0), b: $0.b - SIMD3(0, 0.2, 0), c: $0.c - SIMD3(0, 0.2, 0)) }
        let combinedCoverage = G.mappedCells(floors: lowerPatch + floor, obstacles: furniture, detectedFloor: 0)!
        precondition(combinedCoverage.cells == furnished.cells, "Live floor coverage must expand beyond the initial patch while retaining furniture holes")
        precondition(G.mappedCells(floors: floor, obstacles: [], detectedFloor: 1) == nil,
                     "Do not invent coverage when geometry disagrees with the measured physical floor")
        precondition(G.cell(at: SIMD2(-0.01, -0.01)) == G.Cell(x: -1, z: -1))
        print("Guardian geometry tests passed")
    }
}
