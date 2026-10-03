import Foundation

// Geometry stays independent of ARKit and Metal so the safety decisions can be
// exercised on the host. All coordinates are physical world X/Z, in metres.
enum KLGuardianGeometry {
    typealias Point = SIMD2<Float>
    struct Segment: Equatable { var a: Point; var b: Point }
    struct Cell: Hashable { var x: Int; var z: Int }
    static let cellSize: Float = 0.05
    static let obstaclePadding: Float = 0.05
    struct Triangle: Sendable { var a, b, c: SIMD3<Float> }
    private struct Crossing: Hashable { var cell: Cell; var vertical: Bool }

    static func length(_ p: Point) -> Float { sqrt(p.x * p.x + p.y * p.y) }
    static func distance(_ p: Point, to s: Segment) -> Float {
        let v = s.b - s.a, w = p - s.a
        let d = v.x * v.x + v.y * v.y
        let t = d > 0 ? min(1, max(0, (w.x * v.x + w.y * v.y) / d)) : 0
        return length(p - (s.a + t * v))
    }
    static func segments(_ points: [Point], closed: Bool = true) -> [Segment] {
        guard points.count > 1 else { return [] }
        return (0..<(closed ? points.count : points.count - 1)).map {
            Segment(a: points[$0], b: points[($0 + 1) % points.count])
        }
    }
    static func contains(_ p: Point, polygon: [Point]) -> Bool {
        contains(p, edges: segments(polygon))
    }
    // Even/odd crossings retain obstacle holes after an anchor rotates the
    // outline away from the raster grid used to generate it.
    static func contains(_ p: Point, edges: [Segment]) -> Bool {
        guard edges.count >= 3 else { return false }
        var inside = false
        for s in edges {
            if distance(p, to: s) < 0.001 { return true }
            if (s.a.y > p.y) != (s.b.y > p.y),
               p.x < (s.b.x - s.a.x) * (p.y - s.a.y) / (s.b.y - s.a.y) + s.a.x {
                inside.toggle()
            }
        }
        return inside
    }
    static func cross(_ a: Point, _ b: Point) -> Float { a.x * b.y - a.y * b.x }
    static func validPolygon(_ points: [Point]) -> Bool {
        guard points.count >= 3, points.allSatisfy({ $0.x.isFinite && $0.y.isFinite }) else { return false }
        let edges = segments(points)
        let area = abs(edges.reduce(Float(0)) { $0 + cross($1.a, $1.b) }) / 2
        guard area >= 1, edges.allSatisfy({ length($0.b - $0.a) >= 0.03 }) else { return false }
        for i in edges.indices {
            for j in edges.indices where j > i + 1 && !(i == 0 && j == edges.count - 1) {
                let a = edges[i], b = edges[j]
                let c1 = cross(a.b - a.a, b.a - a.a), c2 = cross(a.b - a.a, b.b - a.a)
                let c3 = cross(b.b - b.a, a.a - b.a), c4 = cross(b.b - b.a, a.b - b.a)
                if c1 * c2 <= 0 && c3 * c4 <= 0,
                   max(min(a.a.x, a.b.x), min(b.a.x, b.b.x)) <= min(max(a.a.x, a.b.x), max(b.a.x, b.b.x)),
                   max(min(a.a.y, a.b.y), min(b.a.y, b.b.y)) <= min(max(a.a.y, a.b.y), max(b.a.y, b.b.y)) { return false }
            }
        }
        return true
    }
    static func cell(at p: Point) -> Cell {
        Cell(x: Int(floor(p.x / cellSize)), z: Int(floor(p.y / cellSize)))
    }
    static func center(_ c: Cell) -> Point { (Point(Float(c.x), Float(c.z)) + 0.5) * cellSize }
    static func connected(_ cells: Set<Cell>, from start: Cell) -> Set<Cell> {
        guard cells.contains(start) else { return [] }
        var visited: Set<Cell> = [start], queue = [start], i = 0
        while i < queue.count {
            let c = queue[i]; i += 1
            for n in [Cell(x: c.x - 1, z: c.z), Cell(x: c.x + 1, z: c.z),
                      Cell(x: c.x, z: c.z - 1), Cell(x: c.x, z: c.z + 1)] {
                if cells.contains(n), visited.insert(n).inserted { queue.append(n) }
            }
        }
        return visited
    }
    static func outline(_ cells: Set<Cell>) -> [Segment] {
        var result: [Segment] = []
        for c in cells {
            let a = Point(Float(c.x), Float(c.z)) * cellSize
            let b = a + Point(cellSize, 0), d = a + Point(0, cellSize), e = a + cellSize
            if !cells.contains(Cell(x: c.x, z: c.z - 1)) { result.append(Segment(a: a, b: b)) }
            if !cells.contains(Cell(x: c.x + 1, z: c.z)) { result.append(Segment(a: b, b: e)) }
            if !cells.contains(Cell(x: c.x, z: c.z + 1)) { result.append(Segment(a: e, b: d)) }
            if !cells.contains(Cell(x: c.x - 1, z: c.z)) { result.append(Segment(a: d, b: a)) }
        }
        return result
    }
    static func coveredCells(_ edges: [Segment]) -> Set<Cell> {
        let points = edges.flatMap { [$0.a, $0.b] }
        guard let minX = points.map(\.x).min(), let maxX = points.map(\.x).max(),
              let minZ = points.map(\.y).min(), let maxZ = points.map(\.y).max(),
              maxX - minX < 20, maxZ - minZ < 20 else { return [] }
        var result: Set<Cell> = []
        for x in Int(floor(minX / cellSize))...Int(floor(maxX / cellSize)) {
            for z in Int(floor(minZ / cellSize))...Int(floor(maxZ / cellSize)) {
                let c = Cell(x: x, z: z)
                if contains(center(c), edges: edges) { result.insert(c) }
            }
        }
        return result
    }
    // Contours contain an outer loop and enclosed obstacle loops. Determine
    // holes by nesting rather than winding, which can differ between sources.
    static func holeLoops(_ edges: [Segment]) -> [[Point]] {
        var adjacent: [Point: [Int]] = [:]
        for (i, edge) in edges.enumerated() where edge.a != edge.b {
            adjacent[edge.a, default: []].append(i)
            adjacent[edge.b, default: []].append(i)
        }
        var visited: Set<Int> = [], loops: [[Point]] = []
        for start in edges.indices where !visited.contains(start) {
            var points: [Point] = [], current = edges[start].a, index = start
            let origin = current
            repeat {
                guard !visited.contains(index), let neighbors = adjacent[current], neighbors.count == 2 else { break }
                visited.insert(index); points.append(current)
                let edge = edges[index]
                current = edge.a == current ? edge.b : edge.a
                guard let next = adjacent[current], next.count == 2 else { break }
                index = next.first { $0 != index }!
            } while current != origin
            if current == origin, points.count >= 3 { loops.append(points) }
        }
        return loops.enumerated().compactMap { i, loop in
            let nesting = loops.enumerated().filter { $0.offset != i && contains(loop[0], polygon: $0.element) }.count
            return nesting.isMultiple(of: 2) ? nil : loop
        }
    }
    static func hole(at point: Point, in holes: [[Point]]) -> [Point]? {
        guard point.x.isFinite, point.y.isFinite else { return nil }
        // A small targeting allowance makes narrow contours easy to select.
        return holes.filter {
            contains(point, polygon: $0) || segments($0).map { distance(point, to: $0) }.min() ?? .infinity < 0.12
        }.min {
            let a = segments($0).map { distance(point, to: $0) }.min() ?? .infinity
            let b = segments($1).map { distance(point, to: $0) }.min() ?? .infinity
            return a < b
        }
    }
    static func removing(_ hole: [Point], from edges: [Segment]) -> [Segment] {
        let removed = segments(hole)
        return edges.filter { edge in
            !removed.contains { ($0.a == edge.a && $0.b == edge.b) || ($0.a == edge.b && $0.b == edge.a) }
        }
    }
    // The field contains clearance to real mapped surfaces minus padding.
    // Shared grid-edge identifiers keep interpolated contours closed, including
    // furniture holes. Ambiguous diagonal contacts stay separate.
    static func contour(_ cells: Set<Cell>, field: [Cell: Float], limiting edges: [Segment] = []) -> [Segment] {
        guard !cells.isEmpty else { return [] }
        var squares: Set<Cell> = []
        for c in cells {
            if [Cell(x: c.x - 1, z: c.z), Cell(x: c.x + 1, z: c.z),
                Cell(x: c.x, z: c.z - 1), Cell(x: c.x, z: c.z + 1)].allSatisfy({ cells.contains($0) }) { continue }
            for dx in -1...0 { for dz in -1...0 { squares.insert(Cell(x: c.x + dx, z: c.z + dz)) } }
        }
        var values: [Cell: Float] = [:]
        func value(_ c: Cell) -> Float {
            if let cached = values[c] { return cached }
            var v = field[c] ?? -cellSize
            if !edges.isEmpty {
                let p = center(c), clearance = edges.map { distance(p, to: $0) }.min() ?? 0
                v = min(v, contains(p, edges: edges) ? clearance : -clearance)
            }
            v = cells.contains(c) ? max(0.00001, v) : -max(0.00001, abs(v))
            values[c] = v; return v
        }
        var points: [Crossing: Point] = [:], neighbors: [Crossing: [Crossing]] = [:]
        func crossing(_ key: Crossing) -> Crossing {
            if points[key] == nil {
                let other = Cell(x: key.cell.x + (key.vertical ? 0 : 1), z: key.cell.z + (key.vertical ? 1 : 0))
                let a = value(key.cell), b = value(other)
                let t = min(1, max(0, a / (a - b)))
                points[key] = center(key.cell) + (center(other) - center(key.cell)) * t
            }
            return key
        }
        func join(_ a: Crossing, _ b: Crossing) {
            neighbors[a, default: []].append(b); neighbors[b, default: []].append(a)
        }
        for c in squares {
            let corners = [c, Cell(x: c.x + 1, z: c.z), Cell(x: c.x + 1, z: c.z + 1), Cell(x: c.x, z: c.z + 1)]
            let signs = corners.map { value($0) > 0 }
            let bits = signs.enumerated().reduce(0) { $0 | ($1.element ? 1 << $1.offset : 0) }
            if bits == 0 || bits == 15 { continue }
            let keys = [Crossing(cell: c, vertical: false), Crossing(cell: corners[1], vertical: true),
                        Crossing(cell: corners[3], vertical: false), Crossing(cell: c, vertical: true)]
            let crossed = (0..<4).filter { signs[$0] != signs[($0 + 1) % 4] }.map { crossing(keys[$0]) }
            if crossed.count == 2 { join(crossed[0], crossed[1]) }
            else if bits == 5 { join(crossing(keys[3]), crossing(keys[0])); join(crossing(keys[1]), crossing(keys[2])) }
            else if bits == 10 { join(crossing(keys[0]), crossing(keys[1])); join(crossing(keys[2]), crossing(keys[3])) }
        }
        func ordered(_ a: Crossing, _ b: Crossing) -> Bool {
            if a.cell.x != b.cell.x { return a.cell.x < b.cell.x }
            if a.cell.z != b.cell.z { return a.cell.z < b.cell.z }
            return !a.vertical && b.vertical
        }
        var visited: Set<Crossing> = [], result: [Segment] = []
        for start in points.keys.sorted(by: ordered) where !visited.contains(start) {
            var loop: [Point] = [], current = start, previous: Crossing?
            repeat {
                guard !visited.contains(current), let p = points[current], let adjacent = neighbors[current], adjacent.count == 2 else { break }
                visited.insert(current); loop.append(p)
                let next = adjacent.sorted(by: ordered).first { $0 != previous }!
                previous = current; current = next
            } while current != start
            if current == start, loop.count >= 3 { result += segments(simplifyLoop(loop, tolerance: 0.0075)) }
        }
        return result
    }
    private static func simplifyLoop(_ points: [Point], tolerance: Float) -> [Point] {
        guard points.count > 3 else { return points }
        let farthest = points.indices.max { length(points[$0] - points[0]) < length(points[$1] - points[0]) }!
        func simplify(_ path: [Point]) -> [Point] {
            guard path.count > 2 else { return path }
            var keep = Set([0, path.count - 1]), stack = [(0, path.count - 1)]
            while let (first, last) = stack.popLast() {
                if last <= first + 1 { continue }
                let segment = Segment(a: path[first], b: path[last])
                var maximum = tolerance, split: Int?
                for i in (first + 1)..<last {
                    let d = distance(path[i], to: segment)
                    if d > maximum { maximum = d; split = i }
                }
                if let split { keep.insert(split); stack += [(first, split), (split, last)] }
            }
            return keep.sorted().map { path[$0] }
        }
        let a = simplify(Array(points[0...farthest]))
        let b = simplify(Array(points[farthest...]) + [points[0]])
        let simplified = Array(a.dropLast()) + Array(b.dropLast())
        return simplified.count >= 3 ? simplified : points
    }
    static func mappedCells(floors: [Triangle], obstacles: [Triangle], detectedFloor: Float? = nil) -> (cells: Set<Cell>, floor: Float, field: [Cell: Float])? {
        let finiteFloors = floors.filter { [$0.a, $0.b, $0.c].allSatisfy { $0.x.isFinite && $0.y.isFinite && $0.z.isFinite } }
        // Triangle density is not floor area. A small, densely reconstructed
        // patch must not choose the height for the entire room.
        let heights = finiteFloors.map { t in
            (height: (t.a.y + t.b.y + t.c.y) / 3,
             area: abs(cross(Point(t.b.x - t.a.x, t.b.z - t.a.z), Point(t.c.x - t.a.x, t.c.z - t.a.z))) / 2)
        }.filter { $0.area > 0.000001 }.sorted { $0.height < $1.height }
        guard !heights.isEmpty else { return nil }
        var accumulated: Float = 0
        let halfArea = heights.reduce(Float(0)) { $0 + $1.area } / 2
        let areaMedian = heights.first { accumulated += $0.area; return accumulated >= halfArea }!.height
        let floor = detectedFloor.flatMap { $0.isFinite ? $0 : nil } ?? areaMedian
        let floors = finiteFloors.filter { [$0.a.y, $0.b.y, $0.c.y].allSatisfy { abs($0 - floor) < 0.12 } }
        guard !floors.isEmpty else { return nil }
        let floorPoints = floors.flatMap { [$0.a, $0.b, $0.c] }
        guard floorPoints.allSatisfy({ $0.x.isFinite && $0.y.isFinite && $0.z.isFinite }) else { return nil }
        let minX = floorPoints.map(\.x).min()!, maxX = floorPoints.map(\.x).max()!
        let minZ = floorPoints.map(\.z).min()!, maxZ = floorPoints.map(\.z).max()!
        guard maxX - minX < 20, maxZ - minZ < 20 else { return nil }
        func visit(_ t: Triangle, padding: Float?, body: (Cell) -> Void) {
            let points = [Point(t.a.x, t.a.z), Point(t.b.x, t.b.z), Point(t.c.x, t.c.z)]
            let margin = (padding ?? 0) + cellSize * 2
            let x0 = max(minX, points.map(\.x).min()! - margin), x1 = min(maxX, points.map(\.x).max()! + margin)
            let z0 = max(minZ, points.map(\.y).min()! - margin), z1 = min(maxZ, points.map(\.y).max()! + margin)
            guard x0 <= x1, z0 <= z1 else { return }
            for x in Int(floorf(x0 / cellSize))...Int(floorf(x1 / cellSize)) {
                for z in Int(floorf(z0 / cellSize))...Int(floorf(z1 / cellSize)) {
                    let cell = Cell(x: x, z: z)
                    if padding != nil || contains(center(cell), polygon: points) { body(cell) }
                }
            }
        }
        var safe: Set<Cell> = []
        for t in floors {
            visit(t, padding: nil) { safe.insert($0) }
        }
        // Sample the actual surface distance, rather than drawing cell boxes.
        // Marching squares interpolates its zero crossing at the 5 cm margin.
        var field = Dictionary(uniqueKeysWithValues: safe.map { ($0, cellSize) })
        for t in obstacles where [t.a, t.b, t.c].allSatisfy({ $0.x.isFinite && $0.y.isFinite && $0.z.isFinite }) {
            let low = min(t.a.y, t.b.y, t.c.y), high = max(t.a.y, t.b.y, t.c.y)
            if high > floor + 0.12 && low < floor + 2 {
                let points = [Point(t.a.x, t.a.z), Point(t.b.x, t.b.z), Point(t.c.x, t.c.z)]
                let perimeter = segments(points)
                visit(t, padding: obstaclePadding) { cell in
                    guard let old = field[cell] else { return }
                    let p = center(cell)
                    let distance = contains(p, polygon: points) ? -obstaclePadding
                        : (perimeter.map { self.distance(p, to: $0) }.min() ?? 0) - obstaclePadding
                    field[cell] = min(old, distance)
                }
            }
        }
        safe = safe.filter { (field[$0] ?? -cellSize) > 0 }
        return (safe, floor, field)
    }
    // Full room visibility before reaching the boundary, with a smooth ramp.
    static func reveal(clearance: Float, warningDistance: Float = 0.3) -> Float {
        guard clearance.isFinite else { return 1 }
        let t = min(1, max(0, (max(0.15, warningDistance) - clearance) / (max(0.15, warningDistance) - 0.05)))
        return t * t * (3 - 2 * t)
    }
}
