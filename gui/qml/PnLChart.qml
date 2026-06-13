import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtCharts 2.15
import HFTEngine 1.0

Item {
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ── Stats row ─────────────────────────────────────────────────────────
        RowLayout {
            spacing: 32

            ColumnLayout {
                Label { text: "Total P&L";    color: "#aaa"; font.pixelSize: 11 }
                Label {
                    text: latencyModel.totalPnl.toFixed(2)
                    color: latencyModel.totalPnl >= 0 ? "#2ecc40" : "#ff4136"
                    font.bold: true
                    font.pixelSize: 16
                }
            }
            ColumnLayout {
                Label { text: "Sharpe";       color: "#aaa"; font.pixelSize: 11 }
                Label { text: latencyModel.sharpe.toFixed(3); color: "white"; font.bold: true; font.pixelSize: 16 }
            }
            ColumnLayout {
                Label { text: "Max Drawdown"; color: "#aaa"; font.pixelSize: 11 }
                Label { text: latencyModel.maxDrawdown.toFixed(2); color: "#ff4136"; font.bold: true; font.pixelSize: 16 }
            }
        }

        // ── P&L line chart ────────────────────────────────────────────────────
        ChartView {
            id: pnlChart
            Layout.fillWidth: true
            Layout.fillHeight: true
            antialiasing: true
            backgroundColor: "#1a1a2e"
            legend.position: ChartView.AlignTop
            animationOptions: ChartView.SeriesAnimations

            DateTimeAxis {
                id: timeAxis
                format: "HH:mm"
                labelsColor: "#aaa"
                gridLineColor: "#333"
            }

            ValueAxis {
                id: pnlAxis
                labelsColor: "#aaa"
                gridLineColor: "#333"
                titleText: "P&L"
            }

            LineSeries {
                id: pnlLine
                name: "P&L"
                axisX: timeAxis
                axisY: pnlAxis
                color: "#4fc3f7"
                width: 2
            }

            LineSeries {
                id: drawdownLine
                name: "Drawdown"
                axisX: timeAxis
                axisY: pnlAxis
                color: "#ff4136"
                width: 1
                style: Qt.DashLine
            }
        }
    }
}
