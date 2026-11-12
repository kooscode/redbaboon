// The controls diagram (briefing and pause menu) and the controller button glyphs: an Xbox Controller S drawn
// at start-up in the HUD's palette, with leader lines from each control to what it does (docs/design.md §3), and
// coloured A / B / X / Y buttons for prompts ("PRESS (A) TO FLY").
#include "game/world.h"

#include "game/canvas.h"

namespace game {

constexpr uint32_t CREAM = 0xFFF4E4C1;
constexpr uint32_t BRASS = 0xFFC9A227;
constexpr int PAD_SIZE = 256;     // the controller texture
constexpr int GLYPH_SIZE = 128;   // the button disc texture
constexpr float PAD_DRAWN = 200.0f; // the controller's size on screen at most

// Points on the controller texture (pixels of the 256 x 256 image) where the leader lines start.
struct PadPoint {
    float x;
    float y;
};

constexpr PadPoint LEFT_TRIGGER = {54.0f, 52.0f};
constexpr PadPoint RIGHT_TRIGGER = {202.0f, 52.0f};
constexpr PadPoint LEFT_STICK = {66.0f, 98.0f};
constexpr PadPoint RIGHT_STICK = {164.0f, 154.0f};
constexpr PadPoint D_PAD = {90.0f, 152.0f};
constexpr PadPoint BUTTON_A = {196.0f, 116.0f};
constexpr PadPoint BUTTON_B = {218.0f, 96.0f};
constexpr PadPoint BUTTON_X = {174.0f, 96.0f};
constexpr PadPoint BUTTON_Y = {196.0f, 76.0f};
constexpr PadPoint BUTTON_START = {138.0f, 130.0f};
constexpr PadPoint BUTTON_BACK = {114.0f, 130.0f};
constexpr PadPoint BUTTON_BLACK = {222.0f, 128.0f};
constexpr PadPoint BUTTON_WHITE = {208.0f, 140.0f};
constexpr float BUTTON_RADIUS = 11.0f;
constexpr float TRIGGER_HALF = 14.0f;

// The body's left and right edges on the texture, and the lanes above and below it for the controls that leave
// it upwards (X, between the triggers) or downwards (start and back, between the grips).
constexpr float BODY_LEFT = 15.0f;
constexpr float BODY_RIGHT = 241.0f;
constexpr float TOP_LANE = 30.0f;
constexpr float BOTTOM_LANE = 240.0f;
constexpr float SIDE_EXIT = -1.0f;

// One label of the diagram: the control, its side, the lane its line leaves the body in (SIDE_EXIT: straight out
// sideways at the control's height), an optional button glyph, what it does and an optional second line, all in
// large text (readable on a TV through composite). Each side is listed top to bottom by where its lines leave the
// body, so no two lines cross.
struct ControlLabel {
    PadPoint point;
    bool right;
    float exitY;
    char glyph;          // 'A', 'B', 'X', 'Y', 'K' (black), 'W' (white): a button glyph before the action; 0 none
    const char* action;
    const char* detail;
};

const ControlLabel LABELS[] = {
    {BUTTON_X, false, TOP_LANE, 'X', "TARGET VIEW", nullptr},
    {LEFT_TRIGGER, false, SIDE_EXIT, 0, "ROCKETS", nullptr},
    {LEFT_STICK, false, SIDE_EXIT, 0, "FLY", nullptr},
    {BUTTON_BACK, false, BOTTOM_LANE, 0, "MISSION", nullptr},
    {RIGHT_TRIGGER, true, SIDE_EXIT, 0, "GUNS", nullptr},
    {BUTTON_Y, true, SIDE_EXIT, 'Y', "LOOK BACK", nullptr},
    {BUTTON_B, true, SIDE_EXIT, 'B', "NEXT TARGET", nullptr},
    {BUTTON_A, true, SIDE_EXIT, 'A', "STUNT", "+ STICK"},
    {BUTTON_BLACK, true, SIDE_EXIT, 'K', "CLOSE CAM", nullptr},
    {BUTTON_WHITE, true, SIDE_EXIT, 'W', "CHASE CAM", nullptr},
    {RIGHT_STICK, true, SIDE_EXIT, 0, "BOOST/BRAKE", "RUDDER"},
    {BUTTON_START, true, BOTTOM_LANE, 0, "PAUSE", nullptr},
};
constexpr int LABEL_COUNT = sizeof(LABELS) / sizeof(LABELS[0]);

// The controller and the button disc textures.
static render::TextureId padTexture = render::NO_TEXTURE;
static render::TextureId glyphTexture = render::NO_TEXTURE;

// --- Art

static uint32_t ButtonColor(char button)
{
    switch (button) {
    case 'A':
        return 0xFF2FA83A;
    case 'B':
        return 0xFFD8352B;
    case 'X':
        return 0xFF2D6FD6;
    case 'K':
        return 0xFF4A4A4A;
    case 'W':
        return 0xFFF0F0F0;
    default:
        return 0xFFE8C22A;
    }
}

// The Controller S: body and grips in dark ink with a brass edge, two sticks, the d-pad, the four face buttons in
// their colours, black and white, start and back, the triggers on top.
static void DrawPad(Canvas& c)
{
    constexpr uint32_t BODY = 0xFF3A332E;
    constexpr uint32_t DARK = 0xFF221D1A;
    // Triggers behind the body.
    const PadPoint& lt = LEFT_TRIGGER;
    const PadPoint& rt = RIGHT_TRIGGER;
    c.Line(lt.x - TRIGGER_HALF, lt.y + 6.0f, lt.x + TRIGGER_HALF, lt.y, 14.0f, BRASS);
    c.Line(rt.x - TRIGGER_HALF, rt.y, rt.x + TRIGGER_HALF, rt.y + 6.0f, 14.0f, BRASS);
    // Brass outline: the same shapes 3 px larger.
    c.Ellipse(128.0f, 104.0f, 113.0f, 51.0f, BRASS);
    c.Ellipse(64.0f, 160.0f, 41.0f, 71.0f, BRASS);
    c.Ellipse(192.0f, 160.0f, 41.0f, 71.0f, BRASS);
    c.Ellipse(128.0f, 104.0f, 110.0f, 48.0f, BODY);
    c.Ellipse(64.0f, 160.0f, 38.0f, 68.0f, BODY);
    c.Ellipse(192.0f, 160.0f, 38.0f, 68.0f, BODY);
    // The jewel in the middle.
    c.Ellipse(128.0f, 92.0f, 15.0f, 15.0f, BRASS);
    c.Ellipse(128.0f, 92.0f, 11.0f, 11.0f, 0xFF2F7A2E);
    // Sticks: a dark well and a cream cap.
    const PadPoint sticks[2] = {LEFT_STICK, RIGHT_STICK};
    for (const PadPoint& s : sticks) {
        c.Ellipse(s.x, s.y, 20.0f, 20.0f, DARK);
        c.Ellipse(s.x, s.y, 13.0f, 13.0f, CREAM);
        c.RingAt(s.x, s.y, 8.0f, 9.5f, 0xFFB8A888);
    }
    // D-pad.
    c.Line(D_PAD.x - 14.0f, D_PAD.y, D_PAD.x + 14.0f, D_PAD.y, 9.0f, DARK);
    c.Line(D_PAD.x, D_PAD.y - 14.0f, D_PAD.x, D_PAD.y + 14.0f, 9.0f, DARK);
    // Face buttons.
    const PadPoint faces[4] = {BUTTON_A, BUTTON_B, BUTTON_X, BUTTON_Y};
    const char names[4] = {'A', 'B', 'X', 'Y'};
    for (int i = 0; i < 4; i++) {
        c.Ellipse(faces[i].x, faces[i].y, BUTTON_RADIUS, BUTTON_RADIUS, ButtonColor(names[i]));
        c.RingAt(faces[i].x, faces[i].y, BUTTON_RADIUS - 2.0f, BUTTON_RADIUS, 0x60FFFFFF);
    }
    // Black and white.
    c.Ellipse(BUTTON_BLACK.x, BUTTON_BLACK.y, 6.0f, 6.0f, 0xFF101010);
    c.Ellipse(BUTTON_WHITE.x, BUTTON_WHITE.y, 6.0f, 6.0f, 0xFFE8E8E8);
    // Start and back.
    c.Line(BUTTON_BACK.x - 4.0f, BUTTON_BACK.y, BUTTON_BACK.x + 4.0f, BUTTON_BACK.y, 6.0f, DARK);
    c.Line(BUTTON_START.x - 4.0f, BUTTON_START.y, BUTTON_START.x + 4.0f, BUTTON_START.y, 6.0f, DARK);
}

void CreateControlsArt()
{
    Canvas pad;
    pad.image.width = PAD_SIZE;
    pad.image.height = PAD_SIZE;
    pad.image.pixels.assign(PAD_SIZE * PAD_SIZE, 0x00000000);
    DrawPad(pad);
    padTexture = render::CreateTexture(pad.image, true, false);

    // A white disc with a soft darker rim and a highlight; tinted per button when drawn.
    Canvas glyph;
    glyph.image.width = GLYPH_SIZE;
    glyph.image.height = GLYPH_SIZE;
    glyph.image.pixels.assign(GLYPH_SIZE * GLYPH_SIZE, 0x00000000);
    float c = GLYPH_SIZE * 0.5f;
    glyph.Ellipse(c, c, c - 2.0f, c - 2.0f, 0xFF9A9A9A);
    glyph.Ellipse(c, c, c - 10.0f, c - 10.0f, 0xFFFFFFFF);
    glyph.Ellipse(c - 14.0f, c - 18.0f, 18.0f, 10.0f, 0x60FFFFFF);
    glyphTexture = render::CreateTexture(glyph.image, true, false);
}

// --- Drawing

void DrawButtonGlyph(char button, float cx, float cy, float size)
{
    float r = size * 0.5f;
    render::DrawImage(glyphTexture, cx - r, cy - r, size, size, 0.0f, 0.0f, 1.0f, 1.0f, ButtonColor(button),
                      render::Blend::ALPHA);
    // Black and white are plain discs.
    if (button == 'K' || button == 'W')
        return;
    char text[2] = {button, '\0'};
    int scale = size >= 30.0f ? 2 : 1;
    float tw = static_cast<float>(render::TextWidth(static_cast<float>(scale), text));
    render::DrawTextShadow(static_cast<float>(static_cast<int>(cx - tw * 0.5f)),
                           static_cast<float>(static_cast<int>(cy - 4.0f * scale)), static_cast<float>(scale),
                           0xFFFFFFFF, text);
}

// One straight piece of a leader line (horizontal or vertical).
static void Segment(float x0, float y0, float x1, float y1)
{
    constexpr uint32_t LINE = 0xD0C9A227;
    render::DrawRect(fminf(x0, x1) - 1.0f, fminf(y0, y1) - 1.0f, fabsf(x1 - x0) + 2.0f, fabsf(y1 - y0) + 2.0f, LINE);
}

// The space from one label's action to the next one's: room for the upper one's second line.
static float RowGap(const ControlLabel& upper)
{
    return upper.detail ? 48.0f : 28.0f;
}

// Where the controller is on screen.
struct PadPlace {
    float x;
    float y;
    float scale;
};

// The labels of one side: rows as close to their lines' exits as the row gap allows, then each line drawn from a
// dot on its control out of the body (sideways, or through its lane), along its own vertical (bus) in the gap
// between the body and the labels when its row is higher or lower, and across to the label.
static void DrawColumn(bool right, float top, float height, const PadPlace& pad)
{
    constexpr float MARGIN = 24.0f;
    constexpr float GLYPH = 22.0f;
    constexpr float LINE_Y = 8.0f;    // the line meets a label at the middle of its action
    const ControlLabel* labels[LABEL_COUNT];
    float exits[LABEL_COUNT];
    float rows[LABEL_COUNT];
    int count = 0;
    float maxWidth = 0.0f;
    for (const ControlLabel& label : LABELS) {
        if (label.right != right)
            continue;
        float glyphWidth = label.glyph ? GLYPH + 6.0f : 0.0f;
        float width = glyphWidth + static_cast<float>(render::TextWidth(2.0f, label.action));
        if (label.detail)
            width = fmaxf(width, glyphWidth + static_cast<float>(render::TextWidth(2.0f, label.detail)));
        float exitY = label.exitY == SIDE_EXIT ? label.point.y : label.exitY;
        labels[count] = &label;
        exits[count] = pad.y + exitY * pad.scale;
        maxWidth = fmaxf(maxWidth, width);
        count++;
    }

    // Rows: pushed down to keep the gap to the row above, then all moved up by the average push, kept on the screen.
    float gaps[LABEL_COUNT];
    for (int i = 1; i < count; i++) {
        gaps[i] = RowGap(*labels[i - 1]);
    }
    float firstRow = top + 10.0f;
    float lastRow = top + height - 26.0f;
    float push = 0.0f;
    for (int i = 0; i < count; i++) {
        float wanted = exits[i] - LINE_Y;
        rows[i] = i == 0 ? fmaxf(wanted, firstRow) : fmaxf(wanted, rows[i - 1] + gaps[i]);
        push += rows[i] - wanted;
    }
    float lift = fminf(push / static_cast<float>(count), rows[0] - firstRow);
    for (int i = count - 1; i >= 0; i--) {
        rows[i] -= lift;
        if (i == count - 1)
            rows[i] = fminf(rows[i], lastRow);
        else
            rows[i] = fminf(rows[i], rows[i + 1] - gaps[i + 1]);
    }

    // Buses: a line going up sits further out than the ones above it; a line going down further out than the ones
    // below it. That keeps every bus clear of the other lines' horizontal pieces.
    int bus[LABEL_COUNT];
    int ups = 0;
    for (int i = 0; i < count; i++) {
        bus[i] = -1;
        if (rows[i] + LINE_Y < exits[i] - 0.5f)
            bus[i] = ups++;
    }
    int downs = 0;
    for (int i = count - 1; i >= 0; i--) {
        if (rows[i] + LINE_Y > exits[i] + 0.5f)
            bus[i] = downs++;
    }
    float w = static_cast<float>(render::ScreenWidth());
    float side = right ? 1.0f : -1.0f;
    float body = pad.x + (right ? BODY_RIGHT : BODY_LEFT) * pad.scale;
    float column = right ? w - MARGIN - maxWidth : MARGIN + maxWidth;
    float lineEnd = column - side * 6.0f;
    float busGap = fabsf(lineEnd - body) / static_cast<float>(ups > downs ? ups + 1 : downs + 1);

    for (int i = 0; i < count; i++) {
        const ControlLabel& label = *labels[i];
        float cx = pad.x + label.point.x * pad.scale;
        float cy = pad.y + label.point.y * pad.scale;
        float y = rows[i] + LINE_Y;
        float x = cx;
        if (label.exitY != SIDE_EXIT) {
            Segment(cx, cy, cx, exits[i]);
        }
        if (bus[i] >= 0) {
            float busX = body + side * busGap * static_cast<float>(bus[i] + 1);
            Segment(x, exits[i], busX, exits[i]);
            Segment(busX, exits[i], busX, y);
            x = busX;
        }
        Segment(x, y, lineEnd, y);
        render::DrawImage(glyphTexture, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 0.0f, 0.0f, 1.0f, 1.0f, BRASS,
                          render::Blend::ALPHA);

        // The label: left of the line's end on the left side, right of it on the right.
        float glyphWidth = label.glyph ? GLYPH + 6.0f : 0.0f;
        float actionWidth = glyphWidth + static_cast<float>(render::TextWidth(2.0f, label.action));
        float actionX = right ? column : column - actionWidth;
        if (label.glyph)
            DrawButtonGlyph(label.glyph, actionX + GLYPH * 0.5f, y, GLYPH);
        HudText(actionX + glyphWidth, rows[i], 2, CREAM, label.action);
        if (label.detail) {
            float detailWidth = static_cast<float>(render::TextWidth(2.0f, label.detail));
            HudText(right ? column + glyphWidth : column - detailWidth, rows[i] + 20.0f, 2, BRASS, label.detail);
        }
    }
}

void DrawControls(float top, float height)
{
    float w = static_cast<float>(render::ScreenWidth());
    float size = fminf(height, PAD_DRAWN);
    PadPlace pad = {(w - size) * 0.5f, top + (height - size) * 0.5f, size / PAD_SIZE};
    render::DrawImage(padTexture, pad.x, pad.y, size, size, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, render::Blend::ALPHA);
    DrawColumn(false, top, height, pad);
    DrawColumn(true, top, height, pad);
}

} // namespace game
