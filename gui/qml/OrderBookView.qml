import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import HFTEngine 1.0

Item {
    // ── Header stats ─────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        spacing: 4
        anchors.margins: 8

        RowLayout {
            spacing: 24

            Label {
                text: "Best Bid: <b>" + (orderBookModel.bestBidPrice / 10000).toFixed(4) + "</b>"
                color: "#2ecc40"
                font.pixelSize: 14
            }
            Label {
                text: "Best Ask: <b>" + (orderBookModel.bestAskPrice / 10000).toFixed(4) + "</b>"
                color: "#ff4136"
                font.pixelSize: 14
            }
            Label {
                text: "Spread: " + (orderBookModel.spread / 10000).toFixed(4)
                color: "white"
                font.pixelSize: 14
            }
        }

        // ── Column headers ────────────────────────────────────────────────────
        RowLayout {
            Label { text: "Side";     color: "#aaa"; Layout.preferredWidth: 50 }
            Label { text: "Price";    color: "#aaa"; Layout.preferredWidth: 100 }
            Label { text: "Quantity"; color: "#aaa"; Layout.fillWidth: true }
        }

        // ── Level list ────────────────────────────────────────────────────────
        ListView {
            id: bookList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: orderBookModel

            delegate: RowLayout {
                width: bookList.width
                height: 22
                spacing: 0

                // Depth bar
                Rectangle {
                    Layout.fillWidth: true
                    height: 18
                    color: model.side === "bid" ? "#1a3a1a" : "#3a1a1a"

                    Rectangle {
                        width: parent.width * (model.depthPercent / 100)
                        height: parent.height
                        color: model.side === "bid" ? "#2ecc40" : "#ff4136"
                        opacity: 0.4
                        anchors.right: model.side === "bid" ? parent.right : undefined
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 4
                        spacing: 8

                        Label {
                            text: model.side.toUpperCase()
                            color: model.side === "bid" ? "#2ecc40" : "#ff4136"
                            font.pixelSize: 11
                            Layout.preferredWidth: 40
                        }
                        Label {
                            text: (model.price / 10000).toFixed(4)
                            color: "white"
                            font.pixelSize: 11
                            font.family: "monospace"
                            Layout.preferredWidth: 90
                        }
                        Label {
                            text: model.quantity
                            color: "#ddd"
                            font.pixelSize: 11
                            font.family: "monospace"
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }
    }
}
