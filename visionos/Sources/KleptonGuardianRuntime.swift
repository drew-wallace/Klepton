import Foundation
#if os(visionOS)
import SwiftUI
#endif

// Settings and anchor-local outlines persist. Session world coordinates are
// restored only after ARKit relocalizes a saved anchor in the physical room.
enum KLGuardianMode: String, CaseIterable, Identifiable {
    case system, manual, automatic
    var id: String { rawValue }
    var title: String {
        switch self {
        case .system: "Default visionOS area"
        case .manual: "Draw on the ground"
        case .automatic: "Mapped environment"
        }
    }
}

// ARKit, spatial events and rendering use different queues. One lock protects
// immutable snapshots; no published SwiftUI properties are touched here.
final class KLGuardianRuntime {
    static let shared = KLGuardianRuntime()
    typealias G = KLGuardianGeometry
    struct Frame {
        var reveal: Float = 1
        var edges: [G.Segment] = []
        var floor: Float = 0
        var showLines = false
        var requiresPassthrough = false
    }
    private let lock = NSLock()
    private var mode: KLGuardianMode = .system
    private var floor: Float?
    private var detectedFloor: Float?
    private var physicalHead: SIMD3<Float>?
    private var warningDistance: Float = 0.3
    private var systemCenter: G.Point?
    private var systemSupplementActive = false
    private var systemSupplementUsed = false
    private var head: G.Point?
    private var polygon: [G.Point] = []
    private var draft: [G.Point] = []
    private var cells: Set<G.Cell> = []
    private var preview: Set<G.Cell> = []
    private var previewEdges: [G.Segment] = []
    private var edges: [G.Segment] = []
    private var drawing = false
    private var accepted = false
    private var message = "Enter immersion to set up your guardian."
    private var tracked = false
    private var mappingHealthy = false
    private var authorized = false
    private var revision = 0
    private var setupRevision = 0
    private var roomID: UUID?
    private var savedAnchorID: UUID?
    private var replacingAnchorID: UUID?
    private var anchorTracked = false
    private var reusePending = false
    private var resumeAccepted = false
    private var removingSpots = false
    private var removedSpots: [[G.Point]] = []
    private var removalCells: Set<G.Cell> = []
    private var selectableSpots: [[G.Point]] = []
    private struct EditState {
        var edges: [G.Segment], previewEdges: [G.Segment]
        var cells: Set<G.Cell>, preview: Set<G.Cell>
        var removedSpots: [[G.Point]]
    }
    private var editOriginal: EditState?
    private var editUndo: [EditState] = []
    #if os(visionOS)
    private var removalEvents: Set<SpatialEventCollection.Event.ID> = []
    #endif

    func select(_ value: KLGuardianMode) {
        lock.lock(); defer { lock.unlock() }
        guard mode != value else { return }
        mode = value; resetLocked()
    }
    func reset() { lock.lock(); resetLocked(); lock.unlock() }
    private func resetLocked() {
        revision += 1; setupRevision += 1
        floor = nil; detectedFloor = nil; physicalHead = nil; systemCenter = nil
        systemSupplementActive = false; systemSupplementUsed = false
        head = nil; tracked = false; mappingHealthy = false; roomID = nil
        polygon = []; draft = []; cells = []; preview = []; previewEdges = []; edges = []
        accepted = false; drawing = false
        savedAnchorID = nil; replacingAnchorID = nil; anchorTracked = false; reusePending = false
        resumeAccepted = false
        resetEditsLocked()
        message = "Looking for a mapped floor. Allow World Sensing to set up your guardian."
    }
    func resetForRenderer() {
        lock.lock(); defer { lock.unlock() }
        // A full/mixed transition may recreate the renderer; the default area
        // remains anchored to the original entry position across that transition.
        if mode != .system { suspendLocked() }
    }
    func beginImmersion() {
        lock.lock(); defer { lock.unlock() }
        if mode == .system { resetLocked() }
        else { suspendLocked() }
    }
    func suspendTracking() {
        lock.lock(); defer { lock.unlock() }
        if mode != .system { suspendLocked() }
    }
    private func suspendLocked() {
        revision += 1; setupRevision += 1
        resumeAccepted = resumeAccepted || (accepted && savedAnchorID != nil)
        accepted = false; tracked = false; mappingHealthy = false; anchorTracked = false
        floor = nil; detectedFloor = nil; head = nil; physicalHead = nil; roomID = nil
        polygon = []; draft = []; cells = []; preview = []; previewEdges = []; edges = []; drawing = false
        resetEditsLocked()
        reusePending = savedAnchorID != nil
        message = "Recognising this room's boundary. Passthrough stays visible until tracking returns."
    }
    private func resumeIfReadyLocked() {
        if resumeAccepted, anchorTracked, tracked, authorized, mappingHealthy, detectedFloor != nil {
            accepted = true; reusePending = false; resumeAccepted = false
            message = "Room boundary restored. Passthrough opens as you approach its edge."
        }
    }
    var referenceHead: SIMD3<Float>? {
        lock.lock(); defer { lock.unlock() }
        return physicalHead
    }
    func setWarningDistance(_ value: Float) {
        lock.lock(); defer { lock.unlock() }
        warningDistance = value.isFinite ? min(0.8, max(0.15, value)) : 0.3
    }
    var suppressInput: Bool {
        lock.lock(); defer { lock.unlock() }
        return mode != .system && (!accepted || drawing || removingSpots || !tracked || !authorized)
    }
    func setAuthorization(_ value: Bool) {
        lock.lock(); defer { lock.unlock() }
        authorized = value
    }
    func unavailable(_ reason: String) {
        lock.lock(); defer { lock.unlock() }
        mappingHealthy = false; message = reason
    }
    func setFloor(_ y: Float) {
        lock.lock(); defer { lock.unlock() }
        guard y.isFinite else { mappingHealthy = false; return }
        detectedFloor = y
        if (!accepted && !drawing) || reusePending { floor = y }
        if mode == .manual { mappingHealthy = true }
        resumeIfReadyLocked()
    }
    func mapped(_ newCells: Set<G.Cell>, floor y: Float, room: UUID, field: [G.Cell: Float]? = nil) {
        lock.lock(); defer { lock.unlock() }
        guard mode == .automatic, y.isFinite else { return }
        if let roomID, roomID != room { suspendLocked() }
        roomID = room; mappingHealthy = true
        guard let head else { return }
        floor = y; detectedFloor = y
        resumeIfReadyLocked()
        // Mapping continues in the background, but never rewrites the outline
        // during a user edit. Only the explicitly deleted holes can reopen.
        guard !removingSpots else { return }
        let newCells = newCells.union(removalCells)
        var field = field
        if field != nil { for cell in removalCells { field![cell] = G.cellSize } }
        if !accepted {
            preview = G.connected(newCells, from: G.cell(at: head))
            previewEdges = field.map { G.contour(preview, field: $0) } ?? G.outline(preview)
        }
        // A live map may shrink an accepted area, but cannot silently expand it.
        if accepted {
            let smaller = cells.intersection(newCells)
            if smaller != cells {
                let contour = field.map { G.contour(smaller, field: $0, limiting: edges) } ?? G.outline(smaller)
                cells = smaller; edges = contour; revision += 1
            }
        } else if savedAnchorID == nil { edges = previewEdges }
        if accepted {
            message = "Mapped boundary active. New obstacles can reduce the play area."
        } else {
            message = preview.isEmpty ? "No clear mapped floor here. Look around the room or use manual setup." : "Review the outlined area, then choose Use mapped area."
        }
    }
    func beginDrawing() {
        lock.lock(); defer { lock.unlock() }
        guard floor != nil, tracked, mappingHealthy, authorized else { return }
        revision += 1; setupRevision += 1
        replacingAnchorID = savedAnchorID
        savedAnchorID = nil; reusePending = false
        resumeAccepted = false
        accepted = false; drawing = true; draft = []; polygon = []; edges = []
        message = "Look at the floor, pinch and trace the perimeter. Release between strokes; Finish closes the outline."
    }
    func undo() {
        lock.lock(); defer { lock.unlock() }
        if !draft.isEmpty { draft.removeLast() }
    }
    func cancelDrawing() {
        lock.lock(); defer { lock.unlock() }
        drawing = false; draft = []; message = "Draw a new boundary before playing."
    }
    func finishDrawing() {
        lock.lock(); defer { lock.unlock() }
        if draft.count > 3, G.length(draft[0] - draft.last!) < 0.08 { draft.removeLast() }
        guard tracked, mappingHealthy, authorized, G.validPolygon(draft), let head, G.contains(head, polygon: draft),
              G.segments(draft).map({ G.distance(head, to: $0) }).min() ?? 0 > 0.25 else {
            message = "Use an outline of at least 1 m² without crossed lines, and stand inside with room to spare."
            return
        }
        polygon = draft; edges = G.segments(polygon); drawing = false; accepted = true
        message = "Drawn boundary active. Passthrough opens as you approach its edge."
    }
    func acceptMap() {
        lock.lock(); defer { lock.unlock() }
        guard let head, tracked, authorized, mappingHealthy, Float(preview.count) * G.cellSize * G.cellSize >= 1,
              edges.map({ G.distance(head, to: $0) }).min() ?? 0 > 0.4 else {
            message = "The mapped area needs more clear floor around you. Look around or use manual setup."
            return
        }
        cells = preview; accepted = true
        edges = previewEdges; resumeAccepted = false; reusePending = false
        message = "Mapped boundary active. Unmapped floor and detected obstacles stay outside the play area."
    }
    func remap() {
        lock.lock(); defer { lock.unlock() }
        guard mode == .automatic else { return }
        revision += 1; setupRevision += 1
        replacingAnchorID = savedAnchorID; savedAnchorID = nil; anchorTracked = false
        accepted = false; reusePending = false; resumeAccepted = false; cells = []
        preview = []; previewEdges = []; edges = []
        resetEditsLocked()
        message = "Review the mapped outline, then choose Use mapped area."
    }
    private func resetEditsLocked() {
        removingSpots = false; removedSpots = []; removalCells = []; selectableSpots = []
        editOriginal = nil; editUndo = []
        #if os(visionOS)
        removalEvents = []
        #endif
    }
    private func editStateLocked() -> EditState {
        EditState(edges: edges, previewEdges: previewEdges, cells: cells, preview: preview, removedSpots: removedSpots)
    }
    private func restoreEditLocked(_ state: EditState) {
        edges = state.edges; previewEdges = state.previewEdges; cells = state.cells; preview = state.preview
        removedSpots = state.removedSpots
        removalCells = Set(removedSpots.flatMap { G.coveredCells(G.segments($0)) })
        selectableSpots = G.holeLoops(edges)
    }
    func beginRemovingSpots() {
        lock.lock(); defer { lock.unlock() }
        guard mode == .automatic, !removingSpots, tracked, authorized, mappingHealthy, floor != nil,
              !reusePending, savedAnchorID == nil || anchorTracked else { return }
        selectableSpots = G.holeLoops(edges)
        guard !selectableSpots.isEmpty else {
            message = "No enclosed spots to remove in this outline."
            return
        }
        setupRevision += 1
        editOriginal = editStateLocked(); editUndo = []; removingSpots = true
        #if os(visionOS)
        removalEvents = []
        #endif
        message = "Look inside an unwanted outlined spot and pinch to remove it. Choose Done when finished."
    }
    func removeSpot(at point: G.Point) {
        lock.lock(); defer { lock.unlock() }
        removeSpotLocked(at: point)
    }
    private func removeSpotLocked(at point: G.Point) {
        guard removingSpots, tracked, authorized, mappingHealthy,
              savedAnchorID == nil || anchorTracked,
              let hole = G.hole(at: point, in: selectableSpots), hole.count <= 4096, removedSpots.count < 256 else { return }
        editUndo.append(editStateLocked())
        if editUndo.count > 32 { editUndo.removeFirst() }
        removedSpots.append(hole)
        let filled = G.coveredCells(G.segments(hole))
        removalCells.formUnion(filled)
        if accepted { cells.formUnion(filled) } else { preview.formUnion(filled) }
        edges = G.removing(hole, from: edges)
        if !accepted { previewEdges = edges }
        selectableSpots = G.holeLoops(edges); revision += 1
        message = "Spot removed. Pinch another unwanted spot, Undo, or choose Done."
    }
    func undoSpotRemoval() {
        lock.lock(); defer { lock.unlock() }
        guard removingSpots, let state = editUndo.popLast() else { return }
        restoreEditLocked(state); revision += 1
        message = "Removal undone. Look inside an unwanted outlined spot and pinch."
    }
    func finishRemovingSpots() {
        lock.lock(); defer { lock.unlock() }
        guard removingSpots, tracked, authorized, mappingHealthy,
              floor != nil, savedAnchorID == nil || anchorTracked else { return }
        removingSpots = false; editOriginal = nil; editUndo = []; selectableSpots = []
        revision += 1
        message = accepted ? "Edited mapped boundary active." : "Review the edited outline, then choose Use mapped area."
    }
    func cancelRemovingSpots() {
        lock.lock(); defer { lock.unlock() }
        guard removingSpots, let state = editOriginal else { return }
        restoreEditLocked(state); removingSpots = false; editOriginal = nil; editUndo = []; selectableSpots = []
        revision += 1
        message = accepted ? "Mapped boundary active. Edits cancelled." : "Review the mapped outline, then choose Use mapped area."
    }
    #if os(visionOS)
    func handle(_ events: SpatialEventCollection) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard drawing || removingSpots else { return false }
        guard let floor, tracked, mappingHealthy, authorized else { return true }
        if removingSpots {
            for event in events where event.phase != .active { removalEvents.remove(event.id) }
        }
        for event in events where event.kind == .indirectPinch && event.phase == .active {
            guard let ray = event.selectionRay else { continue }
            let o = SIMD3<Float>(Float(ray.origin.x), Float(ray.origin.y), Float(ray.origin.z))
            let d = SIMD3<Float>(Float(ray.direction.x), Float(ray.direction.y), Float(ray.direction.z))
            guard d.y < -0.05 else { continue }
            let t = (floor - o.y) / d.y
            guard t > 0, t < 8 else { continue }
            let p = o + d * t, point = G.Point(p.x, p.z)
            if removingSpots {
                if removalEvents.insert(event.id).inserted { removeSpotLocked(at: point) }
            } else if draft.count < 256, draft.last.map({ G.length(point - $0) > 0.06 }) ?? true {
                draft.append(point)
            }
        }
        return true
    }
    #endif
    func frame(head position: SIMD3<Float>, tracked isTracked: Bool,
               controllers: [SIMD3<Float>] = [], controllersTracked: Bool = true) -> Frame {
        lock.lock(); defer { lock.unlock() }
        guard position.x.isFinite, position.y.isFinite, position.z.isFinite else {
            head = nil; physicalHead = nil; tracked = false
            return Frame(reveal: mode == .system && !systemSupplementActive ? 0 : 1,
                         requiresPassthrough: mode != .system || systemSupplementActive)
        }
        physicalHead = position; head = G.Point(position.x, position.z); tracked = isTracked
        resumeIfReadyLocked()
        let validControllers = controllers.filter { $0.x.isFinite && $0.y.isFinite && $0.z.isFinite }
        let controllersHealthy = controllersTracked && validControllers.count == controllers.count
        if mode == .system {
            if systemCenter == nil, isTracked { systemCenter = head }
            guard let center = systemCenter, let head else { return Frame(reveal: 0) }
            let headClearance = 1.5 - G.length(head - center)
            let controllerClearance = validControllers.map { 1.5 - G.length(G.Point($0.x, $0.z) - center) - 0.05 }.min() ?? .infinity
            if !controllersHealthy || controllerClearance < warningDistance ||
               (systemSupplementUsed && headClearance < warningDistance) {
                systemSupplementActive = true; systemSupplementUsed = true
            }
            guard systemSupplementActive else { return Frame(reveal: 0) }
            if isTracked && controllersHealthy && min(headClearance, controllerClearance) > warningDistance + 0.05 {
                systemSupplementActive = false
                return Frame(reveal: 0)
            }
            let reveal = isTracked && controllersHealthy
                ? G.reveal(clearance: min(headClearance, controllerClearance), warningDistance: warningDistance) : 1
            return Frame(reveal: reveal, requiresPassthrough: true)
        }
        let lines = drawing ? G.segments(draft, closed: false) : edges
        var result = Frame(edges: lines, floor: floor ?? 0,
                           showLines: floor != nil && (savedAnchorID == nil || anchorTracked), requiresPassthrough: true)
        guard isTracked, controllersHealthy, authorized, mappingHealthy, accepted, !drawing, !removingSpots,
              savedAnchorID == nil || anchorTracked, let head else { return result }
        func clearance(_ p: G.Point) -> Float {
            let inside = G.contains(p, edges: edges)
            return inside ? edges.map({ G.distance(p, to: $0) }).min() ?? 0 : 0
        }
        let nearest = min(clearance(head), validControllers.map { max(0, clearance(G.Point($0.x, $0.z)) - 0.05) }.min() ?? .infinity)
        result.reveal = G.reveal(clearance: nearest, warningDistance: warningDistance)
        result.showLines = result.reveal > 0
        return result
    }
    func boundaryForSaving() -> (points: [G.Point], removedSpots: [[G.Point]], floor: Float, mode: KLGuardianMode, anchor: UUID?, replacing: UUID?, revision: Int, setupRevision: Int)? {
        lock.lock(); defer { lock.unlock() }
        guard mode != .system, accepted, !removingSpots, tracked, mappingHealthy, authorized, let floor else { return nil }
        let points = mode == .manual ? polygon : edges.flatMap { [$0.a, $0.b] }
        guard !points.isEmpty || (mode == .automatic && savedAnchorID != nil) else { return nil }
        return (points, removedSpots, floor, mode, savedAnchorID, replacingAnchorID, revision, setupRevision)
    }
    func isCurrent(_ value: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return setupRevision == value && mode != .system && accepted && !removingSpots
    }
    func didSave(_ id: UUID, revision value: Int) {
        lock.lock(); defer { lock.unlock() }
        guard revision == value, mode != .system, accepted else { return }
        savedAnchorID = id; anchorTracked = true; replacingAnchorID = nil
        message = "Boundary active and saved for this room. Returning from Home restores it automatically."
    }
    func saveFailed() {
        lock.lock(); defer { lock.unlock() }
        message = "Boundary active for this session. Saving its room anchor failed; set it up again next time."
    }
    func legacyBoundaryFound() {
        lock.lock(); defer { lock.unlock() }
        if mode == .manual, !accepted, !drawing, !reusePending {
            message = "The previous drawing used an older coordinate frame. Draw it once more to save its corrected room position."
        }
    }
    func restore(anchor id: UUID, polygon points: [G.Point], floor y: Float) {
        lock.lock(); defer { lock.unlock() }
        guard mode == .manual, !drawing, y.isFinite, G.validPolygon(points) else { return }
        restoreLocked(anchor: id, polygon: points, edges: G.segments(points), floor: y)
    }
    func restoreMap(anchor id: UUID, edges lines: [G.Segment], floor y: Float, removedSpots spots: [[G.Point]] = []) {
        lock.lock(); defer { lock.unlock() }
        guard mode == .automatic, !removingSpots, replacingAnchorID != id, y.isFinite, lines.isEmpty || lines.count >= 3,
              lines.allSatisfy({ $0.a.x.isFinite && $0.a.y.isFinite && $0.b.x.isFinite && $0.b.y.isFinite }) else { return }
        if let savedAnchorID, savedAnchorID != id { return }
        guard savedAnchorID != nil || (!accepted && head.map { G.contains($0, edges: lines) } == true) else { return }
        if removedSpots != spots {
            removedSpots = spots
            removalCells = Set(spots.flatMap { G.coveredCells(G.segments($0)) })
        }
        restoreLocked(anchor: id, polygon: [], edges: lines, floor: y)
    }
    private func restoreLocked(anchor id: UUID, polygon points: [G.Point], edges lines: [G.Segment], floor y: Float) {
        if let savedAnchorID { guard savedAnchorID == id else { return } }
        else {
            guard !accepted, let head, G.contains(head, edges: lines) else { return }
            savedAnchorID = id; reusePending = true
            // A tracked saved outline already identifies this physical space.
            // Both drawn and automatic boundaries wait for fresh tracking,
            // then apply without another approval after relaunch or mode changes.
            resumeAccepted = true
        }
        polygon = points
        if mode == .automatic, edges != lines { cells = G.coveredCells(lines) }
        edges = lines
        // Saving must not move an active drawing vertically. Reuse is aligned
        // with a freshly detected physical floor, rather than guessed anchor Y.
        if !accepted { floor = detectedFloor ?? y }
        anchorTracked = true
        if mode == .manual { mappingHealthy = true }
        if reusePending {
            message = "Saved boundary recognised. Applying it when tracking is ready."
        }
        resumeIfReadyLocked()
    }
    func anchorLost(_ id: UUID) {
        lock.lock(); defer { lock.unlock() }
        guard savedAnchorID == id else { return }
        if removingSpots, let original = editOriginal {
            restoreEditLocked(original); removingSpots = false; editOriginal = nil; editUndo = []; selectableSpots = []
            revision += 1
        }
        resumeAccepted = resumeAccepted || accepted
        anchorTracked = false; accepted = false; reusePending = true
        message = "Looking for this room's saved boundary. Passthrough stays visible until its anchor is recognised."
    }
    func reuse() {
        lock.lock(); defer { lock.unlock() }
        guard reusePending, anchorTracked, tracked, authorized, mappingHealthy, detectedFloor != nil, let head,
              G.contains(head, edges: edges), edges.map({ G.distance(head, to: $0) }).min() ?? 0 > 0.25 else {
            message = "Stand inside the saved outline with room to spare before reusing it."
            return
        }
        accepted = true; reusePending = false; resumeAccepted = false
        message = "Saved boundary active. Passthrough opens as you approach its edge."
    }
    func uiState() -> (status: String, drawing: Bool, ready: Bool, canDraw: Bool, canAccept: Bool, canReuse: Bool, removingSpots: Bool, canRemoveSpots: Bool, canUndoRemoval: Bool) {
        lock.lock(); defer { lock.unlock() }
        let status = mode == .system ? "visionOS provides its standard area around your starting position." : message
        let canEdit = mode == .automatic && !removingSpots && !edges.isEmpty && !reusePending && tracked && authorized && mappingHealthy && (savedAnchorID == nil || anchorTracked)
        return (status, drawing, accepted && !removingSpots, floor != nil && tracked && mappingHealthy && authorized,
                !removingSpots && !preview.isEmpty && !accepted && !reusePending && tracked && authorized && mappingHealthy,
                reusePending && anchorTracked && tracked && mappingHealthy && authorized && detectedFloor != nil && !resumeAccepted,
                removingSpots, canEdit, removingSpots && !editUndo.isEmpty)
    }
}
