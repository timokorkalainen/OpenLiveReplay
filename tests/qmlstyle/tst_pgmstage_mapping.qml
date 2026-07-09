import QtQuick
import QtQuick.Window
import QtTest
import "../../ui/components"

TestCase {
    id: tc
    name: "PgmStageMapping"
    when: windowShown
    width: 640
    height: 360

    // qmllint disable unqualified
    readonly property var previewUiFixture: previewUi
    // qmllint enable unqualified

    QtObject {
        id: mockUi

        property var viewSlotMap: [1, 0, 2, 3]
        property int multiviewCount: 4
        property int lastPlaybackSingle: -1
        property int lastPlaybackIndex: -99
        property bool playbackSingleView: false
        property int playbackSelectedIndex: -1
        property QtObject multiviewPreviewProvider: null
        property QtObject pgmPreviewProvider: null

        signal playbackProvidersChanged()
        signal streamUrlsChanged()
        signal feedSelectRequested(int index)
        signal multiviewRequested()
        signal playbackViewStateChanged()

        function setPlaybackViewState(singleView, selectedIndex) {
            lastPlaybackSingle = singleView ? 1 : 0
            lastPlaybackIndex = selectedIndex
        }

        function sourceDisplayLabel(sourceIndex) {
            return "SRC" + sourceIndex
        }
    }

    PgmStage {
        id: stage
        width: tc.width
        height: tc.height
        ui: mockUi
    }

    Window {
        id: lifecycleWindow
        width: 320
        height: 180
        visible: true

        PgmStage {
            id: lifecycleStage
            anchors.fill: parent
            ui: tc.previewUiFixture
        }
    }

    function init() {
        mockUi.viewSlotMap = [1, 0, 2, 3]
        stage.resetToMulti()
        mockUi.lastPlaybackSingle = -1
        mockUi.lastPlaybackIndex = -99
    }

    function test_viewSlotSelectionPassesViewSlotToPlayback() {
        stage.selectViewSlot(0)

        compare(stage.viewMode, "single")
        compare(stage.selectedIndex, 0)
        compare(stage.selectedSourceIndex, 1)
        compare(mockUi.lastPlaybackSingle, 1)
        compare(mockUi.lastPlaybackIndex, 0)
    }

    function test_unmappedViewSlotDoesNotSetOnAir() {
        mockUi.viewSlotMap = [-1, 0, 2, 3]

        stage.selectViewSlot(0)

        compare(stage.viewMode, "multi")
        compare(stage.selectedSourceIndex, -1)
        compare(mockUi.lastPlaybackIndex, -99)
    }

    function test_externalFeedSelectionPassesMappedViewSlotToPlayback() {
        mockUi.feedSelectRequested(1)

        compare(stage.viewMode, "single")
        compare(stage.selectedIndex, 0)
        compare(stage.selectedSourceIndex, 1)
        compare(mockUi.lastPlaybackIndex, 0)
    }

    function test_externalFeedSelectionForHiddenSourceDoesNotSelectPgm() {
        mockUi.viewSlotMap = [1, 0, 2, 3]

        mockUi.feedSelectRequested(7)

        compare(stage.viewMode, "multi")
        compare(stage.selectedIndex, -1)
        compare(stage.selectedSourceIndex, -1)
        compare(mockUi.lastPlaybackIndex, -99)
    }

    function test_externalPlaybackViewStateDrivesVisibleSingleView() {
        mockUi.playbackSingleView = true
        mockUi.playbackSelectedIndex = 0

        mockUi.playbackViewStateChanged()

        compare(stage.viewMode, "single")
        compare(stage.selectedIndex, 0)
        compare(stage.selectedSourceIndex, 1)
        compare(mockUi.lastPlaybackIndex, -99)
    }

    function test_activeMultiviewReattachesAfterProvidersAreReplaced() {
        lifecycleStage.resetToMulti()

        tc.previewUiFixture.replacePreviewProviders()

        tryCompare(tc.previewUiFixture, "multiviewHasConsumer", true)
    }

    function test_pgmAttachesWhenSelectedAfterProvidersAreReplaced() {
        lifecycleStage.resetToMulti()
        tc.previewUiFixture.replacePreviewProviders()
        lifecycleStage.selectViewSlot(0)

        tryCompare(tc.previewUiFixture, "pgmHasConsumer", true)
    }

    function test_viewSlotMapChangeRebindsSelectedSourceToNewViewSlot() {
        stage.selectSource(1)
        compare(stage.viewMode, "single")
        compare(stage.selectedIndex, 0)
        compare(mockUi.lastPlaybackIndex, 0)

        mockUi.viewSlotMap = [0, 1, 2, 3]
        mockUi.viewSlotMapChanged()

        compare(stage.viewMode, "single")
        compare(stage.selectedIndex, 1)
        compare(stage.selectedSourceIndex, 1)
        compare(mockUi.lastPlaybackIndex, 1)
    }

    function test_viewSlotMapChangeResetsHiddenSelectedSource() {
        stage.selectSource(1)
        compare(stage.viewMode, "single")

        mockUi.viewSlotMap = [0, 2, 3, -1]
        mockUi.viewSlotMapChanged()

        compare(stage.viewMode, "multi")
        compare(stage.selectedIndex, -1)
        compare(stage.selectedSourceIndex, -1)
    }
}
