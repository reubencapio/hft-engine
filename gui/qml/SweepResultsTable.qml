import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import HFTEngine 1.0

Item {
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ── Toolbar ───────────────────────────────────────────────────────────
        RowLayout {
            Label { text: sweepResultModel.totalResults + " results"; color: "#aaa" }
            Item { Layout.fillWidth: true }
            Button {
                text: "Load CSV..."
                onClicked: fileDialog.open()
            }
            BusyIndicator {
                running: sweepResultModel.isLoading
                visible: sweepResultModel.isLoading
                implicitWidth: 24
                implicitHeight: 24
            }
        }

        // ── Column headers ────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            height: 28
            color: "#2a2a3e"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 4
                spacing: 0

                Repeater {
                    model: ["Spread", "Qty", "Lookback(ms)", "Sharpe", "Drawdown", "P&L", "Fill%", "Lat(ns)"]
                    delegate: Label {
                        text: modelData
                        color: "#aaa"
                        font.pixelSize: 11
                        font.bold: true
                        Layout.preferredWidth: 90
                        MouseArea {
                            anchors.fill: parent
                            onClicked: sweepResultModel.sortByColumn(index)
                        }
                    }
                }
            }
        }

        // ── Result rows ───────────────────────────────────────────────────────
        ListView {
            id: resultList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: sweepResultModel

            delegate: Rectangle {
                width: resultList.width
                height: 24
                color: {
                    if (model.sharpe > 1.5) return "#1a3a1a"
                    if (model.sharpe > 0.5) return "#2a2a1a"
                    return "#2a1a1a"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 4
                    spacing: 0

                    Label { text: model.spread_ticks;                           font.pixelSize: 11; color: "white"; Layout.preferredWidth: 90 }
                    Label { text: model.order_qty;                              font.pixelSize: 11; color: "white"; Layout.preferredWidth: 90 }
                    Label { text: (model.lookback_ns / 1e6).toFixed(0);        font.pixelSize: 11; color: "white"; Layout.preferredWidth: 90 }
                    Label {
                        text: model.sharpe.toFixed(3)
                        color: model.sharpe > 1.5 ? "#2ecc40" : (model.sharpe > 0.5 ? "#f0c040" : "#ff4136")
                        font.bold: true; font.pixelSize: 11; Layout.preferredWidth: 90
                    }
                    Label { text: model.max_drawdown.toFixed(2);               font.pixelSize: 11; color: "#ff4136"; Layout.preferredWidth: 90 }
                    Label {
                        text: model.total_pnl.toFixed(2)
                        color: model.total_pnl >= 0 ? "#2ecc40" : "#ff4136"
                        font.pixelSize: 11; Layout.preferredWidth: 90
                    }
                    Label { text: (model.fill_rate * 100).toFixed(1) + "%";    font.pixelSize: 11; color: "#aaa"; Layout.preferredWidth: 90 }
                    Label { text: model.avg_latency_ns.toFixed(0);             font.pixelSize: 11; color: "#aaa"; Layout.fillWidth: true }
                }
            }
        }
    }

    // ── File dialog (simplified — real impl uses platform dialog) ─────────────
    Dialog {
        id: fileDialog
        title: "Open CSV File"
        standardButtons: Dialog.Ok | Dialog.Cancel

        TextField {
            id: pathField
            width: 400
            placeholderText: "Path to sweep_results.csv"
        }

        onAccepted: {
            if (pathField.text.length > 0)
                sweepResultModel.loadFromCSV(pathField.text)
        }
    }
}
