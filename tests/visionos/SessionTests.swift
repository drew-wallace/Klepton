import XCTest

final class SessionTests: XCTestCase {
    @MainActor func testQuitBeforeBoot() {
        let app = XCUIApplication(bundleIdentifier: "drewwallace.dev.klepton.target.walkabout-57013")
        app.launchEnvironment = ["KL_AUTOBOOT": "0", "KL_IMMERSIVE": "0"]
        app.launch()
        let quit = app.buttons["quitKlepton"]
        XCTAssertTrue(quit.waitForExistence(timeout: 15))
        quit.tap()
        XCTAssertTrue(app.wait(for: .notRunning, timeout: 15), "Quit should terminate Klepton even before a game starts")
    }

    @MainActor func testQuitRunningGameAndRelaunch() {
        let app = XCUIApplication(bundleIdentifier: "drewwallace.dev.klepton.target.walkabout-57013")
        // No Steam-mode or camera-height overrides: exercise Home-launch defaults.
        app.launchEnvironment = [:]
        for _ in 1...3 {
            app.launch()
            // Give the asset/shader load time to reach the menu after the logo.
            Thread.sleep(forTimeInterval: 35)
            XCTAssertTrue(app.wait(for: .runningForeground, timeout: 15))
            let quit = app.buttons["quitKlepton"]
            XCTAssertTrue(quit.waitForExistence(timeout: 15))
            quit.tap()
            XCTAssertTrue(app.wait(for: .notRunning, timeout: 15), "Quit should terminate Klepton and the running game")
        }
    }

    @MainActor func testHomeAndReopen() {
        let app = XCUIApplication(bundleIdentifier: "drewwallace.dev.klepton.target.walkabout-57013")
        app.launchEnvironment = [:]
        app.launch()
        Thread.sleep(forTimeInterval: 10)
        XCTAssertTrue(app.wait(for: .runningForeground, timeout: 15))
        XCTAssertTrue(app.buttons["Matting"].exists || app.staticTexts["Matting"].exists,
                      "Klepton settings window should remain open on cold boot")
        XCTAssertTrue(app.buttons["quitKlepton"].exists, "Quit should be available on cold boot")
        XCTAssertFalse(app.buttons["Resume session"].exists, "The game should start automatically on cold boot")
        for cycle in 1...3 {
            XCUIDevice.shared.press(.home)
            // With Klepton's window visible, the first Crown press leaves
            // immersion for Shared Space; the next opens Home View.
            Thread.sleep(forTimeInterval: 1)
            XCUIDevice.shared.press(.home)
            Thread.sleep(forTimeInterval: 1)
            XCTAssertNotEqual(app.state, .notRunning, "Session exited on Home, cycle \(cycle)")
            app.activate()
            XCTAssertTrue(app.wait(for: .runningForeground, timeout: 15))
            Thread.sleep(forTimeInterval: 4)
            XCTAssertTrue(app.buttons["Matting"].exists || app.staticTexts["Matting"].exists,
                          "Klepton settings window should remain open after re-entry")
            XCTAssertFalse(app.buttons["Resume session"].exists,
                           "The game should resume automatically alongside its settings window")
            XCTAssertTrue(app.buttons["quitKlepton"].exists, "Quit should remain available after re-entry")
        }
    }
}
