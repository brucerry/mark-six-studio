import QtQuick

Item {
    id: chart
    property var primaryValues: []
    property var comparisonValues: []
    property bool showZero: true
    property string primaryLabel: "Observed"
    property string comparisonLabel: "Reference"
    property string firstLabel: "earliest"
    readonly property bool hasData: primaryValues.length > 1
    onPrimaryValuesChanged: canvas.requestPaint()
    onComparisonValuesChanged: canvas.requestPaint()
    onShowZeroChanged: canvas.requestPaint()

    function label(value) {
        if (Math.abs(value) < 0.0000001) return "0"
        if (Math.abs(value) >= 100) return value.toFixed(0)
        if (Math.abs(value) >= 1) return value.toFixed(2)
        if (Math.abs(value) >= 0.01) return value.toFixed(3)
        if (Math.abs(value) >= 0.0001) return value.toFixed(5)
        return value.toExponential(1)
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (!chart.hasData || width < 120 || height < 70) return

            const left = 58, right = width - 12, top = 10, bottom = height - 24
            const series = [chart.primaryValues, chart.comparisonValues]
            let low = chart.showZero ? 0 : Infinity
            let high = chart.showZero ? 0 : -Infinity
            for (const points of series) {
                for (const point of points) {
                    const value = Number(point)
                    if (Number.isFinite(value)) {
                        low = Math.min(low, value)
                        high = Math.max(high, value)
                    }
                }
            }
            if (!Number.isFinite(low) || !Number.isFinite(high)) return
            const range = high - low
            const padding = range > 0 ? range * 0.08 : Math.max(Math.abs(high) * 0.08, 0.000001)
            low -= padding
            high += padding
            const y = value => top + (high - value) * (bottom - top) / (high - low)

            ctx.font = "11px sans-serif"
            ctx.fillStyle = "#9bb0c9"
            ctx.strokeStyle = "#314359"
            ctx.lineWidth = 1
            for (let tick = 0; tick <= 2; ++tick) {
                const value = high - tick * (high - low) / 2
                const py = top + tick * (bottom - top)
                ctx.beginPath()
                ctx.moveTo(left, py)
                ctx.lineTo(right, py)
                ctx.stroke()
                ctx.fillText(chart.label(value), 2, py + 4)
            }
            if (chart.showZero && low < 0 && high > 0) {
                ctx.strokeStyle = "#9bb0c9"
                ctx.setLineDash([4, 5])
                ctx.beginPath()
                ctx.moveTo(left, y(0))
                ctx.lineTo(right, y(0))
                ctx.stroke()
                ctx.setLineDash([])
            }

            function draw(points, color, dashed) {
                if (points.length < 2) return
                // Many draws share one pixel. Average each pixel-sized bucket so
                // dense history reads as a trend instead of a wall of strokes.
                const buckets = Math.max(2, Math.floor((right - left) / 3))
                const bucketSize = Math.max(1, Math.ceil(points.length / buckets))
                ctx.strokeStyle = color
                ctx.lineWidth = 2
                ctx.lineJoin = "round"
                ctx.setLineDash(dashed ? [6, 5] : [])
                ctx.beginPath()
                let started = false
                for (let start = 0; start < points.length; start += bucketSize) {
                    const end = Math.min(points.length, start + bucketSize)
                    let sum = 0, count = 0
                    for (let i = start; i < end; ++i) {
                        const value = Number(points[i])
                        if (Number.isFinite(value)) { sum += value; ++count }
                    }
                    if (!count) continue
                    const px = left + ((start + end - 1) / 2) * (right - left) / (points.length - 1)
                    const py = y(sum / count)
                    if (!started) { ctx.moveTo(px, py); started = true }
                    else ctx.lineTo(px, py)
                }
                ctx.stroke()
                ctx.setLineDash([])
            }
            draw(chart.comparisonValues, "#e9b866", true)
            draw(chart.primaryValues, "#55d6c1", false)
            ctx.fillStyle = "#9bb0c9"
            ctx.fillText(chart.firstLabel, left, height - 5)
            ctx.fillText("latest", right - 30, height - 5)
        }
    }

    Text {
        anchors.centerIn: parent
        visible: !chart.hasData
        text: "No progress series available"
        color: StudioTheme.muted
        font.pixelSize: 13
    }
    Accessible.role: Accessible.StaticText
    Accessible.name: primaryLabel + (comparisonValues.length ? " and " + comparisonLabel : "") +
        " over saved draws. Exact values are in the learning table."
}
