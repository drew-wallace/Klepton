// swiftc visionos/Sources/KleptonGuardianGeometry.swift \
//   visionos/Sources/KleptonGuardianRuntime.swift tests/visionos/GuardianRuntimeTests.swift \
//   -o /tmp/guardian-runtime-tests && /tmp/guardian-runtime-tests
import Foundation

@main struct GuardianRuntimeTests {
    static func main() {
        typealias G = KLGuardianGeometry
        let runtime = KLGuardianRuntime(), anchor = UUID()
        let square: [G.Point] = [SIMD2(-2, -2), SIMD2(2, -2), SIMD2(2, 2), SIMD2(-2, 2)]
        runtime.select(.manual); runtime.setAuthorization(true)
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 1)
        runtime.restore(anchor: anchor, polygon: square, floor: 0)
        precondition(!runtime.uiState().ready && !runtime.uiState().canReuse)
        precondition(runtime.suppressInput)
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 1, "A saved drawing must wait for a fresh physical floor")
        runtime.setFloor(0)
        precondition(runtime.uiState().ready && !runtime.uiState().canReuse, "A saved drawing must reapply without confirmation when tracking is ready")
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 0)
        precondition(!runtime.suppressInput)
        let fade = runtime.frame(head: SIMD3(1.8, 1.6, 0), tracked: true).reveal
        precondition(fade > 0 && fade < 1)
        precondition(runtime.frame(head: SIMD3(3, 1.6, 0), tracked: true).reveal == 1)
        precondition(runtime.frame(head: .zero, tracked: false).reveal == 1)
        precondition(runtime.frame(head: SIMD3(.nan, 1.6, 0), tracked: true).reveal == 1, "Invalid head coordinates must reveal the room")
        runtime.anchorLost(anchor)
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 1)
        runtime.restore(anchor: anchor, polygon: square, floor: 0)
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 0, "An already approved anchor resumes after tracking recovery")
        runtime.setAuthorization(false)
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 1)
        runtime.setAuthorization(true)
        let shifted = square.map { $0 + SIMD2<Float>(4, 0) }
        runtime.restore(anchor: anchor, polygon: shifted, floor: 0)
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 1, "A new anchor transform must move the boundary")
        // Anchor updates must not change the vertical placement of an active drawing.
        runtime.restore(anchor: anchor, polygon: square, floor: -1.5)
        precondition(runtime.frame(head: .zero, tracked: true).floor == 0)
        precondition(runtime.frame(head: .zero, tracked: true,
            controllers: [SIMD3(1.8, 1, 0), SIMD3(-0.5, 1, 0)]).reveal > 0)
        precondition(runtime.frame(head: .zero, tracked: true,
            controllers: [SIMD3(-2.1, 1, 0)]).reveal == 1, "Either controller can cross the outline while the head stays inside")
        precondition(runtime.frame(head: .zero, tracked: true,
            controllers: [SIMD3(0.5, 1, 0)]).reveal == 0, "A controller well inside must not reveal passthrough")
        precondition(runtime.frame(head: .zero, tracked: true, controllersTracked: false).reveal == 1)
        runtime.setWarningDistance(0.15)
        precondition(runtime.frame(head: .zero, tracked: true,
            controllers: [SIMD3(1.7, 1, 0)]).reveal == 0, "A shorter warning distance must suppress an early warning")
        runtime.setWarningDistance(0.3)
        runtime.beginImmersion()
        let awaitingRoom = runtime.frame(head: SIMD3(5, 1.6, 3), tracked: true)
        precondition(awaitingRoom.reveal == 1 && !awaitingRoom.showLines, "Never display the previous immersion's coordinates")
        let turned = square.map { SIMD2<Float>(-$0.y + 5, $0.x + 3) }
        runtime.restore(anchor: anchor, polygon: turned, floor: -1.4)
        precondition(!runtime.uiState().ready, "A room anchor alone cannot resume before a fresh floor")
        runtime.setFloor(-1.4)
        let resumed = runtime.frame(head: SIMD3(5, 1.6, 3), tracked: true)
        precondition(runtime.uiState().ready && resumed.reveal == 0 && resumed.edges == G.segments(turned))
        precondition(runtime.frame(head: SIMD3(5, 1.6, 3), tracked: true,
            controllers: [SIMD3(5, 1, 4.85)]).reveal > 0, "Controller checks use the restored physical outline")
        runtime.reset()
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 1, "Never reuse session coordinates after re-entry")
        // Cold restoration automatically applies the drawing at the freshly
        // measured floor height rather than the saved anchor's old height.
        runtime.setFloor(-1.4)
        runtime.restore(anchor: anchor, polygon: square, floor: -2.8)
        precondition(runtime.frame(head: .zero, tracked: true).floor == -1.4)
        precondition(runtime.frame(head: .zero, tracked: true).reveal == 0 && runtime.uiState().ready && !runtime.uiState().canReuse)
        let drawnAwaitingHead = KLGuardianRuntime()
        drawnAwaitingHead.select(.manual); drawnAwaitingHead.setAuthorization(true)
        _ = drawnAwaitingHead.frame(head: .zero, tracked: false)
        drawnAwaitingHead.setFloor(0)
        drawnAwaitingHead.restore(anchor: anchor, polygon: square, floor: 0)
        precondition(!drawnAwaitingHead.uiState().ready && !drawnAwaitingHead.uiState().canReuse)
        precondition(drawnAwaitingHead.frame(head: .zero, tracked: true).reveal == 0 && drawnAwaitingHead.uiState().ready,
                     "A drawn boundary automatically resumes when head tracking arrives last")
        let drawnWithoutPermission = KLGuardianRuntime()
        drawnWithoutPermission.select(.manual)
        _ = drawnWithoutPermission.frame(head: .zero, tracked: true)
        drawnWithoutPermission.setFloor(0)
        drawnWithoutPermission.restore(anchor: anchor, polygon: square, floor: 0)
        precondition(!drawnWithoutPermission.uiState().ready && drawnWithoutPermission.frame(head: .zero, tracked: true).reveal == 1)
        drawnWithoutPermission.setAuthorization(true)
        precondition(drawnWithoutPermission.frame(head: .zero, tracked: true).reveal == 0 && !drawnWithoutPermission.uiState().canReuse,
                     "Automatic reuse of a drawing must wait for permission")
        let drawnElsewhere = KLGuardianRuntime()
        drawnElsewhere.select(.manual); drawnElsewhere.setAuthorization(true)
        _ = drawnElsewhere.frame(head: SIMD3(20, 1.6, 20), tracked: true)
        drawnElsewhere.setFloor(0)
        drawnElsewhere.restore(anchor: anchor, polygon: square, floor: 0)
        precondition(!drawnElsewhere.uiState().ready && drawnElsewhere.frame(head: SIMD3(20, 1.6, 20), tracked: true).reveal == 1,
                     "A drawing from elsewhere must not automatically activate")
        drawnAwaitingHead.beginDrawing()
        drawnAwaitingHead.restore(anchor: anchor, polygon: square, floor: 0)
        precondition(drawnAwaitingHead.uiState().drawing && !drawnAwaitingHead.uiState().ready,
                     "Saved drawing updates must not interrupt a new drawing")
        runtime.select(.automatic)
        _ = runtime.frame(head: SIMD3(0.975, 1.6, 0.975), tracked: true)
        let cells = Set((0..<42).flatMap { x in (0..<42).map { G.Cell(x: x, z: $0) } })
        let room = UUID()
        runtime.mapped(cells, floor: 0, room: room); runtime.acceptMap()
        precondition(runtime.frame(head: SIMD3(0.975, 1.6, 0.975), tracked: true).reveal == 0)
        precondition(runtime.frame(head: SIMD3(0.975, 1.6, 0.975), tracked: true,
            controllers: [SIMD3(2.2, 1, 1)]).reveal == 1, "Mapped areas also check controllers")
        var withHole = cells; withHole.remove(G.cell(at: SIMD2(0.975, 0.975)))
        let savingSetup = runtime.boundaryForSaving()!.setupRevision
        runtime.mapped(withHole, floor: 0, room: room)
        precondition(runtime.isCurrent(savingSetup), "Obstacle reductions during anchor creation must not cancel saving the approved area")
        precondition(runtime.frame(head: SIMD3(0.975, 1.6, 0.975), tracked: true).reveal == 1)
        runtime.mapped(cells, floor: 0, room: room)
        precondition(runtime.frame(head: SIMD3(0.975, 1.6, 0.975), tracked: true).reveal == 1, "A map update cannot silently reopen excluded floor")
        let mapAnchor = UUID()
        runtime.didSave(mapAnchor, revision: runtime.boundaryForSaving()!.revision)
        let approvedEdges = runtime.frame(head: SIMD3(0.975, 1.6, 0.975), tracked: true).edges
        runtime.beginImmersion()
        let turn: Float = 0.6
        func move(_ p: G.Point) -> G.Point {
            SIMD2(cos(turn) * p.x - sin(turn) * p.y + 4, sin(turn) * p.x + cos(turn) * p.y - 2)
        }
        let centerAfterHome = move(SIMD2(0.975, 0.975))
        let newCenter = SIMD3<Float>(centerAfterHome.x, 1.6, centerAfterHome.y)
        precondition(!runtime.frame(head: newCenter, tracked: true).showLines)
        let movedEdges = approvedEdges.map { G.Segment(a: move($0.a), b: move($0.b)) }
        runtime.restoreMap(anchor: mapAnchor, edges: movedEdges, floor: 0)
        runtime.setFloor(0)
        precondition(!runtime.uiState().ready, "A restored mapped area must wait for room mapping")
        let movedCells = G.coveredCells(movedEdges)
        runtime.mapped(movedCells, floor: 0, room: room)
        let mapResumed = runtime.frame(head: newCenter, tracked: true)
        precondition(runtime.uiState().ready && mapResumed.edges == movedEdges)
        precondition(mapResumed.reveal == 1, "Restored obstacle holes still trigger passthrough")
        let safePoint = move(SIMD2(0.6, 0.6))
        let safeHead = SIMD3<Float>(safePoint.x, 1.6, safePoint.y)
        precondition(runtime.frame(head: safeHead, tracked: true).reveal == 0)
        runtime.resetForRenderer()
        precondition(runtime.frame(head: safeHead, tracked: true).reveal == 1)
        runtime.setFloor(0)
        runtime.mapped(movedCells, floor: 0, room: room)
        runtime.restoreMap(anchor: mapAnchor, edges: movedEdges, floor: 0)
        precondition(runtime.frame(head: safeHead, tracked: true).reveal == 0, "A renderer restart restores the accepted mapped area")
        runtime.mapped(cells, floor: 0, room: UUID())
        precondition(!runtime.uiState().ready, "Changing rooms requires fresh setup")
        // A recognised automatic area applies on a fresh launch without a
        // reuse prompt, matching the saved-drawing behavior above.
        let relaunched = KLGuardianRuntime()
        relaunched.select(.automatic); relaunched.setAuthorization(true)
        _ = relaunched.frame(head: safeHead, tracked: true)
        relaunched.mapped(movedCells, floor: 0, room: room)
        relaunched.restoreMap(anchor: mapAnchor, edges: movedEdges, floor: 0)
        precondition(relaunched.frame(head: safeHead, tracked: true).reveal == 0 && relaunched.uiState().ready && !relaunched.uiState().canReuse,
                     "A saved automatic boundary must apply without confirmation after relaunch")
        let awaitingMap = KLGuardianRuntime()
        awaitingMap.select(.automatic); awaitingMap.setAuthorization(true)
        _ = awaitingMap.frame(head: safeHead, tracked: true)
        awaitingMap.restoreMap(anchor: mapAnchor, edges: movedEdges, floor: 0)
        precondition(!awaitingMap.uiState().ready && !awaitingMap.uiState().canReuse && awaitingMap.suppressInput)
        awaitingMap.setFloor(0)
        precondition(awaitingMap.frame(head: safeHead, tracked: true).reveal == 1,
                     "A tracked saved anchor and floor alone cannot resume before room mapping")
        awaitingMap.mapped(movedCells, floor: 0, room: room)
        precondition(awaitingMap.uiState().ready && !awaitingMap.uiState().canReuse && awaitingMap.frame(head: safeHead, tracked: true).reveal == 0)
        let unknownSpace = KLGuardianRuntime()
        unknownSpace.select(.automatic); unknownSpace.setAuthorization(true)
        _ = unknownSpace.frame(head: SIMD3(30, 1.6, 30), tracked: true)
        unknownSpace.mapped(movedCells, floor: 0, room: UUID())
        unknownSpace.restoreMap(anchor: mapAnchor, edges: movedEdges, floor: 0)
        precondition(!unknownSpace.uiState().ready && unknownSpace.boundaryForSaving() == nil && unknownSpace.frame(head: SIMD3(30, 1.6, 30), tracked: true).reveal == 1,
                     "An outline from elsewhere must not become the current boundary")
        relaunched.mapped([], floor: 0, room: room)
        precondition(relaunched.boundaryForSaving()!.points.isEmpty, "A completely excluded area must also update its saved geometry")
        relaunched.restoreMap(anchor: mapAnchor, edges: [], floor: 0)
        relaunched.mapped(movedCells, floor: 0, room: room)
        precondition(relaunched.frame(head: safeHead, tracked: true).reveal == 1, "Restoring an emptied outline cannot silently reopen it")
        let contoured = KLGuardianRuntime()
        contoured.select(.automatic); contoured.setAuthorization(true)
        _ = contoured.frame(head: SIMD3(0.6, 1.6, 1.5), tracked: true)
        let floorTriangles: [G.Triangle] = [
            .init(a: SIMD3(0, 0, 0), b: SIMD3(3, 0, 0), c: SIMD3(3, 0, 3)),
            .init(a: SIMD3(0, 0, 0), b: SIMD3(3, 0, 3), c: SIMD3(0, 0, 3))]
        let wallTriangles: [G.Triangle] = [.init(a: SIMD3(1.3, 0, 0), b: SIMD3(1.3, 1, 0), c: SIMD3(1.3, 1, 3))]
        let surfaceMap = G.mappedCells(floors: floorTriangles, obstacles: wallTriangles)!
        contoured.mapped(surfaceMap.cells, floor: 0, room: room, field: surfaceMap.field)
        let preview = contoured.frame(head: SIMD3(0.6, 1.6, 1.5), tracked: true).edges
        contoured.acceptMap()
        let approvedContour = contoured.frame(head: SIMD3(0.6, 1.6, 1.5), tracked: true)
        precondition(approvedContour.edges == preview && approvedContour.reveal == 0, "Applying an automatic area must retain its interpolated contour")
        precondition(contoured.frame(head: SIMD3(0.6, 1.6, 1.5), tracked: true,
            controllers: [SIMD3(1.26, 1, 1.5)]).reveal == 1, "The controller warning must use that same contour")
        // Removing a false obstacle is an explicit edit, distinct from live
        // mapping's rule that accepted areas can only shrink.
        let editing = KLGuardianRuntime()
        editing.select(.automatic); editing.setAuthorization(true)
        let editHead = SIMD3<Float>(0.5, 1.6, 0.5), spot = SIMD2<Float>(1.5, 1.5)
        _ = editing.frame(head: editHead, tracked: true)
        let falseObstacle: [G.Triangle] = [
            .init(a: SIMD3(1, 0.8, 1), b: SIMD3(2, 0.8, 1), c: SIMD3(2, 0.8, 2)),
            .init(a: SIMD3(1, 0.8, 1), b: SIMD3(2, 0.8, 2), c: SIMD3(1, 0.8, 2))]
        let spotMap = G.mappedCells(floors: floorTriangles, obstacles: falseObstacle)!
        editing.mapped(spotMap.cells, floor: 0, room: room, field: spotMap.field); editing.acceptMap()
        let original = editing.frame(head: editHead, tracked: true).edges
        let editAnchor = UUID()
        editing.didSave(editAnchor, revision: editing.boundaryForSaving()!.revision)
        editing.beginRemovingSpots()
        precondition(editing.uiState().removingSpots && editing.suppressInput)
        precondition(editing.frame(head: editHead, tracked: true).reveal == 1 && editing.boundaryForSaving() == nil,
                     "Editing must expose the real room and keep pending changes out of persistence")
        editing.removeSpot(at: SIMD2(0, 1.5))
        precondition(editing.frame(head: editHead, tracked: true).edges == original)
        editing.removeSpot(at: spot)
        precondition(editing.uiState().canUndoRemoval && G.contains(spot, edges: editing.frame(head: editHead, tracked: true).edges))
        editing.restoreMap(anchor: editAnchor, edges: original, floor: 0)
        precondition(G.contains(spot, edges: editing.frame(head: editHead, tracked: true).edges), "Anchor polling must not overwrite a pending edit")
        editing.mapped(spotMap.cells, floor: 0, room: room, field: spotMap.field)
        precondition(G.contains(spot, edges: editing.frame(head: editHead, tracked: true).edges), "Live maps must not interrupt an edit")
        editing.undoSpotRemoval()
        precondition(editing.frame(head: editHead, tracked: true).edges == original)
        editing.removeSpot(at: spot); editing.cancelRemovingSpots()
        precondition(editing.frame(head: editHead, tracked: true).edges == original && !editing.suppressInput)
        editing.beginRemovingSpots(); editing.removeSpot(at: spot); editing.finishRemovingSpots()
        let edited = editing.boundaryForSaving()!
        precondition(edited.removedSpots.count == 1 && editing.uiState().ready)
        precondition(editing.frame(head: editHead, tracked: true, controllers: [SIMD3(1.5, 1, 1.5)]).reveal == 0)
        editing.mapped(spotMap.cells, floor: 0, room: room, field: spotMap.field)
        precondition(editing.frame(head: SIMD3(1.5, 1.6, 1.5), tracked: true).reveal == 0,
                     "A deleted spot must not return on the next raw map update")
        precondition(editing.frame(head: SIMD3(3.1, 1.6, 1.5), tracked: true).reveal == 1)
        editing.didSave(editAnchor, revision: editing.boundaryForSaving()!.revision)
        let editedEdges = editing.frame(head: editHead, tracked: true).edges
        let editedSpots = editing.boundaryForSaving()!.removedSpots
        editing.beginImmersion()
        let movedHead = move(SIMD2(0.5, 0.5))
        _ = editing.frame(head: SIMD3(movedHead.x, 1.6, movedHead.y), tracked: true)
        let restoredEdges = editedEdges.map { G.Segment(a: move($0.a), b: move($0.b)) }
        let restoredSpots = editedSpots.map { $0.map(move) }
        editing.restoreMap(anchor: editAnchor, edges: restoredEdges, floor: 0, removedSpots: restoredSpots)
        editing.setFloor(0)
        let rotatedRaw = G.coveredCells(original.map { G.Segment(a: move($0.a), b: move($0.b)) })
        editing.mapped(rotatedRaw, floor: 0, room: room)
        let movedSpot = move(spot)
        precondition(editing.uiState().ready && editing.frame(head: SIMD3(movedSpot.x, 1.6, movedSpot.y), tracked: true).reveal == 0,
                     "Deleted spots must stay deleted after a rotated Home re-entry")
        let editedRelaunch = KLGuardianRuntime()
        editedRelaunch.select(.automatic); editedRelaunch.setAuthorization(true)
        let clearedSpotHead = SIMD3<Float>(movedSpot.x, 1.6, movedSpot.y)
        _ = editedRelaunch.frame(head: clearedSpotHead, tracked: true)
        editedRelaunch.mapped(rotatedRaw, floor: 0, room: room)
        editedRelaunch.restoreMap(anchor: editAnchor, edges: restoredEdges, floor: 0, removedSpots: restoredSpots)
        editedRelaunch.mapped(rotatedRaw, floor: 0, room: room)
        precondition(editedRelaunch.uiState().ready && !editedRelaunch.uiState().canReuse && editedRelaunch.frame(head: clearedSpotHead, tracked: true).reveal == 0,
                     "Automatic reuse after relaunch must preserve deleted spots even when the wearer stands in one")
        editing.remap()
        editing.restoreMap(anchor: editAnchor, edges: restoredEdges, floor: 0, removedSpots: restoredSpots)
        precondition(!editing.uiState().canReuse, "Polling must not restore the boundary that an explicit remap is replacing")
        editing.mapped(rotatedRaw, floor: 0, room: room)
        precondition(!G.contains(movedSpot, edges: editing.frame(head: SIMD3(movedHead.x, 1.6, movedHead.y), tracked: true).edges),
                     "An explicit remap discards removal overrides")
        // Preview edits are still subject to the normal explicit apply action.
        let previewEdit = KLGuardianRuntime()
        previewEdit.select(.automatic); previewEdit.setAuthorization(true)
        _ = previewEdit.frame(head: editHead, tracked: true)
        previewEdit.mapped(spotMap.cells, floor: 0, room: room, field: spotMap.field)
        previewEdit.beginRemovingSpots(); previewEdit.removeSpot(at: spot)
        _ = previewEdit.frame(head: editHead, tracked: false)
        previewEdit.finishRemovingSpots()
        precondition(previewEdit.uiState().removingSpots, "Do not commit an edit while tracking is unavailable")
        _ = previewEdit.frame(head: editHead, tracked: true)
        previewEdit.finishRemovingSpots()
        precondition(!previewEdit.uiState().ready && previewEdit.uiState().canAccept && previewEdit.boundaryForSaving() == nil)
        previewEdit.acceptMap()
        precondition(previewEdit.uiState().ready && previewEdit.boundaryForSaving()!.removedSpots.count == 1)
        let previewAnchor = UUID()
        previewEdit.didSave(previewAnchor, revision: previewEdit.boundaryForSaving()!.revision)
        // A new obstruction outside the ignored spot must still shrink the area.
        let stillBlocked = spotMap.cells.filter { $0.x > 4 }
        previewEdit.mapped(Set(stillBlocked), floor: 0, room: room, field: spotMap.field)
        precondition(previewEdit.frame(head: SIMD3(0.1, 1.6, 0.5), tracked: true).reveal == 1)
        runtime.select(.system)
        precondition(runtime.frame(head: .zero, tracked: false).reveal == 0, "The system owns its own boundary")
        _ = runtime.frame(head: SIMD3(4, 1.6, 2), tracked: true)
        let systemWarn = runtime.frame(head: SIMD3(4, 1.6, 2), tracked: true,
            controllers: [SIMD3(5.35, 1, 2)])
        precondition(systemWarn.reveal > 0 && systemWarn.requiresPassthrough)
        precondition(runtime.frame(head: SIMD3(4, 1.6, 2), tracked: true,
            controllers: [SIMD3(5.6, 1, 2)]).reveal == 1)
        runtime.resetForRenderer()
        precondition(runtime.frame(head: SIMD3(4.1, 1.6, 2), tracked: true,
            controllers: [SIMD3(5.6, 1, 2)]).reveal == 1, "Style changes must not recenter the default controller area")
        let systemClear = runtime.frame(head: SIMD3(4, 1.6, 2), tracked: true,
            controllers: [SIMD3(4.5, 1, 2)])
        precondition(systemClear.reveal == 0 && !systemClear.requiresPassthrough)
        precondition(runtime.frame(head: SIMD3(4, 1.6, 2), tracked: true,
            controllersTracked: false).requiresPassthrough)
        print("Guardian runtime tests passed")
    }
}
