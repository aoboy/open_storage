#!/usr/bin/env python3
"""
Generates: storage_paper_stack_design.pdf
Requires: pip install reportlab
"""

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import cm, mm
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, PageBreak, Table, TableStyle
)
from reportlab.graphics.shapes import Drawing, Rect, String, Line, Polygon
from reportlab.graphics import renderPDF

OUT = "storage_paper_stack_design.pdf"

styles = getSampleStyleSheet()
H1 = ParagraphStyle("H1", parent=styles["Heading1"], fontSize=16, spaceAfter=8)
H2 = ParagraphStyle("H2", parent=styles["Heading2"], fontSize=13, spaceAfter=6)
BODY = ParagraphStyle("Body", parent=styles["BodyText"], fontSize=9.5, leading=13)
SMALL = ParagraphStyle("Small", parent=styles["BodyText"], fontSize=8, leading=11)


def make_diagram():
    """Vector-native three-plane architecture diagram."""
    w, h = 17*cm, 13*cm
    d = Drawing(w, h)

    # Plane backgrounds
    planes = [
        (0, 8.8*cm, "SENDER / SOURCE PLANE", colors.HexColor("#E8F0FE")),
        (0, 4.6*cm, "WAN / INTERCONNECT PLANE", colors.HexColor("#FFF3E0")),
        (0, 0.4*cm, "RECEIVER / APPLY PLANE", colors.HexColor("#E8F5E9")),
    ]
    for x, y, label, fill in planes:
        d.add(Rect(x, y, w, 4.0*cm, fillColor=fill, strokeColor=colors.HexColor("#B0BEC5")))
        d.add(String(x + 0.2*cm, y + 3.6*cm, label, fontSize=8, fontName="Helvetica-Bold", fillColor=colors.HexColor("#37474F")))

    # Sender boxes
    sx = [1.0*cm, 6.0*cm, 11.0*cm]
    sender_labels = ["Origin A\n(S3/Blob)", "Origin B\n(CDN Edge)", "P2P Peer /\nColocated"]
    for i, (x, label) in enumerate(zip(sx, sender_labels)):
        d.add(Rect(x, 9.2*cm, 3.2*cm, 2.4*cm, fillColor=colors.white, strokeColor=colors.HexColor("#5C6BC0"), strokeWidth=1.2))
        d.add(String(x + 1.6*cm, 10.4*cm, label, fontSize=7.5, textAnchor="middle", fillColor=colors.HexColor("#1A237E")))

    # Whack-a-Mole dispatch box
    d.add(Rect(5.5*cm, 9.5*cm, 6.0*cm, 1.8*cm, fillColor=colors.HexColor("#C5CAE9"), strokeColor=colors.HexColor("#3949AB"), strokeWidth=1.5))
    d.add(String(8.5*cm, 10.7*cm, "Whack-a-Mole Dispatch", fontSize=8, textAnchor="middle", fontName="Helvetica-Bold", fillColor=colors.HexColor("#1A237E")))
    d.add(String(8.5*cm, 10.1*cm, "discrete profile + bit-reversal counter + soft shedding", fontSize=6.5, textAnchor="middle", fillColor=colors.HexColor("#283593")))

    # WAN paths
    wy = 5.2*cm
    for i, (x, label) in enumerate(zip([1.5*cm, 6.5*cm, 11.5*cm], ["Path 1\n(healthy)", "Path 2\n(degraded)", "Path k\n(healthy)"])):
        stroke = colors.HexColor("#E53935") if i == 1 else colors.HexColor("#43A047")
        d.add(Rect(x, wy, 3.2*cm, 2.0*cm, fillColor=colors.white, strokeColor=stroke, strokeWidth=1.2))
        d.add(String(x + 1.6*cm, wy + 1.0*cm, label, fontSize=7, textAnchor="middle", fillColor=colors.HexColor("#263238")))

    # FLUID box
    d.add(Rect(5.5*cm, 5.3*cm, 6.0*cm, 1.8*cm, fillColor=colors.HexColor("#FFE0B2"), strokeColor=colors.HexColor("#EF6C00"), strokeWidth=1.5))
    d.add(String(8.5*cm, 6.5*cm, "FLUID Slack Repair", fontSize=8, textAnchor="middle", fontName="Helvetica-Bold", fillColor=colors.HexColor("#BF360C")))
    d.add(String(8.5*cm, 5.9*cm, "fountain decode gate; finish when Loss-Product < ε", fontSize=6.5, textAnchor="middle", fillColor=colors.HexColor("#BF360C")))

    # ARC box
    d.add(Rect(5.5*cm, 1.0*cm, 6.0*cm, 1.8*cm, fillColor=colors.HexColor("#C8E6C9"), strokeColor=colors.HexColor("#2E7D32"), strokeWidth=1.5))
    d.add(String(8.5*cm, 2.2*cm, "ARC Receiver-Side Pacing", fontSize=8, textAnchor="middle", fontName="Helvetica-Bold", fillColor=colors.HexColor("#1B5E20")))
    d.add(String(8.5*cm, 1.6*cm, "adaptive release offset; no feedback; receiver-clock only", fontSize=6.5, textAnchor="middle", fillColor=colors.HexColor("#1B5E20")))

    # Engine box
    d.add(Rect(5.5*cm, 1.0*cm, 6.0*cm, 0.0*cm, fillColor=colors.white, strokeColor=colors.white))
    # Actually place engine below ARC within receiver plane
    d.add(Rect(12.5*cm, 1.0*cm, 3.5*cm, 1.8*cm, fillColor=colors.white, strokeColor=colors.HexColor("#2E7D32"), strokeWidth=1.2))
    d.add(String(14.25*cm, 1.9*cm, "Storage Engine /\nApply Path", fontSize=7, textAnchor="middle", fillColor=colors.HexColor("#1B5E20")))

    # Arrows (simplified)
    def arrow(x1, y1, x2, y2, color=colors.HexColor("#546E7A")):
        d.add(Line(x1, y1, x2, y2, strokeColor=color, strokeWidth=1.2))
        # small arrowhead
        d.add(Polygon([x2, y2, x2-0.15*cm, y2+0.1*cm, x2-0.15*cm, y2-0.1*cm], fillColor=color, strokeColor=color))

    for x in sx:
        arrow(x + 1.6*cm, 9.2*cm, 8.5*cm, 11.3*cm, colors.HexColor("#5C6BC0"))
    arrow(8.5*cm, 9.5*cm, 8.5*cm, 7.3*cm, colors.HexColor("#5C6BC0"))
    arrow(8.5*cm, 5.2*cm, 8.5*cm, 3.0*cm, colors.HexColor("#EF6C00"))
    arrow(8.5*cm, 1.0*cm, 12.5*cm, 1.9*cm, colors.HexColor("#2E7D32"))

    return d


def build():
    doc = SimpleDocTemplate(
        OUT, pagesize=A4,
        leftMargin=2*cm, rightMargin=2*cm,
        topMargin=2*cm, bottomMargin=2*cm,
        title="Paper-to-Storage Stack: Whack-a-Mole, FLUID, ARC",
        author="Senior R&D Technical Staff Analysis",
    )
    story = []

    story.append(Paragraph("Applying Whack-a-Mole, FLUID, and ARC to Large-Scale Distributed Storage &amp; Caching", H1))
    story.append(Paragraph("Internal design note — prototype sequencing, architecture, and Modal Storage MTS fit", SMALL))
    story.append(Spacer(1, 0.4*cm))

    story.append(Paragraph("1. Executive summary", H2))
    story.append(Paragraph(
        "The three papers form a coherent stack for deterministic, coded, receiver-paced data movement. "
        "Whack-a-Mole determines <i>where</i> each unit goes (discrete path profile with bounded discrepancy). "
        "FLUID determines <i>when to stop</i> sending/recovering (slack-based fountain decoding instead of all-or-nothing ARQ). "
        "ARC determines <i>when to release</i> at the receiver (timing-preserving pacing without feedback). "
        "Applied to storage, they attack cache-fill skew, cross-region EC repair tail, and bursty replication apply.", BODY))
    story.append(Spacer(1, 0.3*cm))

    story.append(Paragraph("2. Architecture diagram", H2))
    story.append(Paragraph(
        "Three-plane architecture: sender/source dispatch, WAN repair, and receiver-side apply pacing. "
        "Whack-a-Mole profiles origins/peers; FLUID gates decode sufficiency on repair; ARC smooths release into the engine.", BODY))
    story.append(Spacer(1, 0.2*cm))
    story.append(make_diagram())
    story.append(Spacer(1, 0.2*cm))
    story.append(Paragraph(
        "<b>Figure 1.</b> Three-plane paper-to-storage stack. Soft shedding in Whack-a-Mole reduces allocation to degraded paths; "
        "FLUID finishes repair when the Loss-Product falls below ε; ARC releases decoded data on the receiver clock without feedback.", SMALL))

    story.append(PageBreak())

    story.append(Paragraph("3. Prototype sequence (ROI/risk order)", H2))
    proto_data = [
        ["#", "Prototype", "Scope", "Primary metric", "Risk"],
        ["1", "Profile-based dispatch + soft shedding for cache-fill striping",
         "Proxy data plane", "Cache-fill p99; completion-time variance; bytes fetched (neutral)",
         "Low: toggleable, no persistent state"],
        ["2", "Slack-based fountain-coded EC repair over WAN",
         "Repair pipeline (isolated)", "Repair p99 vs. bandwidth overhead Pareto curve",
         "Medium: new coding path, but A/B-able per repair job"],
        ["3", "ARC-style pacing sidecar in one ingest/replication pipeline",
         "Receiver/apply plane (engine-adjacent)", "Engine p99 write latency; leader backpressure event rate",
         "Higher: touches write/apply critical path; single pipeline first"],
    ]
    t = Table(proto_data, colWidths=[0.8*cm, 4.0*cm, 3.0*cm, 4.2*cm, 3.5*cm])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#37474F")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, -1), 7.5),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#B0BEC5")),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#F5F5F5")]),
    ]))
    story.append(t)
    story.append(Spacer(1, 0.3*cm))

    story.append(Paragraph("3.1 Prototype 1 details", H2))
    story.append(Paragraph(
        "Replace existing hash/round-robin cache-fill dispatch with a discrete Whack-a-Mole profile over origins (S3, CDN edge, P2P peer, colocated cluster). "
        "Represent profile as integer selection units b(i)/m; select path via bit-reversal counter with O(log m) per-packet cost and bounded discrepancy. "
        "Apply soft shedding: gradually reduce b(i) when a target shows elevated latency/error/queue depth, and redistribute to healthy targets. "
        "Success: lower cache-fill p99 and completion-time variance with no increase in total bytes fetched.", BODY))

    story.append(Paragraph("3.2 Prototype 2 details", H2))
    story.append(Paragraph(
        "Introduce fountain-coded repair fragments and a decode-sufficiency gate. Delivery finishes when the Loss-Product across rounds falls below ε. "
        "Expose ε as an operator knob and plot repair p99 vs. bandwidth overhead relative to ARQ. "
        "Because repair is a separate pipeline, route a configurable fraction of repair jobs through FLUID and compare against the ARQ control group. "
        "Expected outcome: substantial tail-latency reduction at a small, tunable overhead premium.", BODY))

    story.append(Paragraph("3.3 Prototype 3 details", H2))
    story.append(Paragraph(
        "Insert an ARC-style sidecar between the replication recovery buffer and the engine apply API in one pipeline (e.g., follower apply). "
        "The sidecar maintains an adaptive release offset tracking the upper envelope of recovery times and releases data in a smoothed stream on the receiver clock. "
        "No feedback to the leader; no transport changes. Measure engine p99 write latency and leader backpressure event rate before/after.", BODY))

    story.append(PageBreak())

    story.append(Paragraph("4. Fit to Modal Storage MTS (Stockholm)", H2))
    story.append(Paragraph(
        "Modal’s storage team operates a distributed object storage system underpinning container images, volumes, and checkpoints: hundreds of petabytes, "
        "replicated across multiple cloud object stores and a CDN, cached on local NVMe, and shared P2P within each datacenter. The stated goals include "
        "making cold starts feel local, hiding object-store latency, keeping public ingress off saturated uplinks, and owning durability and cost at petabyte scale.", BODY))
    story.append(Spacer(1, 0.2*cm))

    fit_data = [
        ["Modal Storage goal", "Paper-stack application"],
        ["Cold starts feel local", "Whack-a-Mole dispatch across origins/peers; ARC pacing for large image/checkpoint ingest into NVMe"],
        ["Keep ingress off saturated uplinks", "Soft shedding in profile dispatch preferentially drains via P2P/colocated clusters"],
        ["Durability & cost at petabyte scale", "FLUID slack EC repair tightens repair tail at bounded overhead; reduces cross-region egress cost exposure"],
        ["P2P sharing within datacenter", "Deterministic profile-based piece selection across peers with degradation-aware redistribution"],
        ["Replication across blob providers", "ARC receiver-side pacing reduces engine p99/backpressure without sender feedback loops"],
    ]
    t2 = Table(fit_data, colWidths=[6.0*cm, 9.5*cm])
    t2.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#1B5E20")),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, -1), 8),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#A5D6A7")),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#F1F8E9")]),
    ]))
    story.append(t2)
    story.append(Spacer(1, 0.3*cm))
    story.append(Paragraph(
        "The role asks for engineers who have built high-performance distributed storage/caching at large scale, worked on caching/preloading/P2P layers, "
        "owned durability and cost at petabyte scale, and worked across the stack from local disk/page cache to distributed blob storage. "
        "The prototype sequence above is a direct demonstration of that profile: a data-plane dispatch change, a repair-plane protocol change, "
        "and an apply-plane pacing change, each scoped to a storage SLO outcome.", BODY))

    doc.build(story)
    print(f"Wrote {OUT}")


if __name__ == "__main__":
    build()

