import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import HFTEngine 1.0

ApplicationWindow {
    id: root
    title: "HFT Engine Dashboard"
    width: 1280
    height: 800
    visible: true

    // ── Tab bar ─────────────────────────────────────────────────────────────
    header: TabBar {
        id: tabBar
        TabButton { text: "Order Book" }
        TabButton { text: "Latency" }
        TabButton { text: "P&L" }
        TabButton { text: "Sweep Results" }
    }

    // ── Page stack ───────────────────────────────────────────────────────────
    StackLayout {
        anchors.fill: parent
        currentIndex: tabBar.currentIndex

        OrderBookView {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        LatencyChart {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        PnLChart {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
        SweepResultsTable {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }

    // ── Status bar ───────────────────────────────────────────────────────────
    footer: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8

            Label {
                id: statusLabel
                text: "Ready"
                Layout.fillWidth: true
            }
            Label {
                text: Qt.formatDateTime(new Date(), "hh:mm:ss")
                font.pixelSize: 11
                color: "gray"
            }
            Item { width: 8 }
        }
    }
}
