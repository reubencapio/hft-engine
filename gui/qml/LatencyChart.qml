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

        // ── Percentile summary ────────────────────────────────────────────────
        RowLayout {
            spacing: 32
            Repeater {
                model: [
                    { label: "p50",  value: latencyModel.p50 },
                    { label: "p99",  value: latencyModel.p99 },
                    { label: "p999", value: latencyModel.p999 }
                ]
                delegate: ColumnLayout {
                    Label {
                        text: modelData.label
                        color: "#aaa"
                        font.pixelSize: 11
                    }
                    Label {
                        text: modelData.value.toFixed(0) + " ns"
                        color: "white"
                        font.bold: true
                        font.pixelSize: 16
                    }
                }
            }
        }

        // ── Bar chart ─────────────────────────────────────────────────────────
        ChartView {
            id: chart
            Layout.fillWidth: true
            Layout.fillHeight: true
            antialiasing: true
            backgroundColor: "#1a1a2e"
            legend.visible: false
            animationOptions: ChartView.SeriesAnimations

            BarSeries {
                id: latencySeries
                BarSet {
                    id: latencyBars
                    color: "#4fc3f7"
                    borderColor: "#4fc3f7"
                    values: latencyModel.bucketCounts
                }
            }

            ValueAxis {
                id: xAxis
                min: 0
                max: latencyModel.bucketCount
                labelFormat: ""
            }

            ValueAxis {
                id: yAxis
                min: 0
                max: Math.max.apply(null, latencyModel.bucketCounts) || 1
                titleText: "Count"
                labelsColor: "#aaa"
                gridLineColor: "#333"
            }
        }

        Label {
            text: "Latency buckets (log₂ scale in ns)"
            color: "#aaa"
            font.pixelSize: 10
            Layout.alignment: Qt.AlignHCenter
        }
    }
}
