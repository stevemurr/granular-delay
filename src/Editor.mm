#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "Editor.h"
#include "Parameters.h"
#include <cfloat>
#include <cmath>
#include <cstring>

using namespace granular;

// ---------------------------------------------------------------- palette & text
static NSColor* color(unsigned rgb, CGFloat alpha = 1) {
    return [NSColor colorWithSRGBRed:((rgb>>16)&255)/255. green:((rgb>>8)&255)/255. blue:(rgb&255)/255. alpha:alpha];
}
static const unsigned kBackground = 0x10141B, kPanel = 0x151A22, kLine = 0x262E3A, kTrack = 0x232A35;
static const unsigned kAmber = 0xE5B76A, kEmber = 0xE59A6A, kSky = 0x82B8F0, kTeal = 0x72CEB4;
static const unsigned kViolet = 0xB9A0E5, kSteel = 0xA4B6D4, kIce = 0x8FD3F4, kRose = 0xE58A7A;
static const unsigned kText = 0xEEF1F5, kDim = 0x8693A3, kFaint = 0x5B6778;

static void drawText(NSString* string, NSRect rect, CGFloat size, NSColor* tint, NSFontWeight weight,
                     CGFloat kern, NSTextAlignment alignment) {
    auto* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = alignment; style.lineBreakMode = NSLineBreakByClipping;
    [string drawInRect:rect withAttributes:@{
        NSFontAttributeName: [NSFont systemFontOfSize:size weight:weight],
        NSForegroundColorAttributeName: tint, NSKernAttributeName: @(kern), NSParagraphStyleAttributeName: style}];
}

// ---------------------------------------------------------------- value formatting
static NSString* valueText(uint32_t pid, double v) {
    switch (pid) {
    case Time:
        if (v >= 1000) return [NSString stringWithFormat:@"%.2f s", v/1000];
        return [NSString stringWithFormat:v >= 100 ? @"%.0f ms" : @"%.1f ms", v];
    case Tone: return v >= 1000 ? [NSString stringWithFormat:@"%.1f kHz", v/1000] : [NSString stringWithFormat:@"%.0f Hz", v];
    case Output: return [NSString stringWithFormat:@"%+.1f dB", v];
    case Division: return [NSString stringWithUTF8String:divisionNames[static_cast<size_t>(std::clamp(v, 0., 8.))]];
    default: break;
    }
    if (std::strcmp(parameters[pid].unit, "%") == 0) return [NSString stringWithFormat:@"%.0f %%", v];
    return [NSString stringWithFormat:@"%.2f", v];
}
// Accepts "250", "250 ms", "1.5 s", "4k", "-6 dB", or a division name.
static bool parseValue(uint32_t pid, NSString* text, double& out) {
    if (pid == Division) {
        for (size_t i = 0; i < divisionNames.size(); ++i)
            if ([text caseInsensitiveCompare:[NSString stringWithUTF8String:divisionNames[i]]] == NSOrderedSame) { out = i; return true; }
    }
    const char* string = text.UTF8String;
    char* end = nullptr;
    double value = std::strtod(string, &end);
    if (end == string || !std::isfinite(value)) return false;
    while (*end == ' ') ++end;
    if (pid == Time && (*end == 's' || *end == 'S')) value *= 1000;
    if (pid == Tone && (*end == 'k' || *end == 'K')) value *= 1000;
    out = sanitize(pid, value);
    return true;
}
static NSString* stackText(double interval, size_t preset) {
    const auto& p = voicePresets[preset];
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    for (size_t i = 0; i < p.count; ++i) {
        const double st = p.semitones[i]*interval/100;
        [parts addObject:std::abs(st) < 1 && st != 0 ? [NSString stringWithFormat:@"%+.2f", st]
            : std::abs(st-std::round(st)) < .05 ? [NSString stringWithFormat:@"%+.0f", st] : [NSString stringWithFormat:@"%+.1f", st]];
    }
    return [[parts componentsJoinedByString:@"   "] stringByAppendingString:@"  st"];
}

// ---------------------------------------------------------------- host protocol
@protocol GDHost <NSObject>
- (double)valueFor:(uint32_t)pid;
- (void)begin:(uint32_t)pid;
- (void)set:(uint32_t)pid value:(double)value;
- (void)end:(uint32_t)pid;
- (void)commit:(uint32_t)pid value:(double)value; // begin + set + end
@end

// ---------------------------------------------------------------- knob
@interface GDKnob : NSView <NSTextFieldDelegate>
@property (weak) id<GDHost> host;
@property (nonatomic) uint32_t param;
@property NSString* title;
@property NSColor* tint;
@property CGFloat labelShift;
@property (readonly) NSTextField* field;
- (void)sync;
@end
@implementation GDKnob {
    double pos_, cont_, lastY_;
    BOOL interacting_;
}
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _field = [[NSTextField alloc] initWithFrame:NSMakeRect(4, 10, frame.size.width-8, 20)];
    _field.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium];
    _field.textColor = color(kText); _field.backgroundColor = NSColor.clearColor;
    _field.drawsBackground = NO; _field.bordered = NO; _field.bezeled = NO;
    _field.alignment = NSTextAlignmentCenter; _field.focusRingType = NSFocusRingTypeNone;
    _field.delegate = self; _field.target = self; _field.action = @selector(typed:);
    [self addSubview:_field];
    return self;
}
- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)setParam:(uint32_t)param {
    if (_param != param) { _param = param; cont_ = -1; }
}
- (BOOL)stepped { return parameters[_param].stepped; }
- (double)positionFor:(double)value {
    return self.stepped ? std::clamp(value/parameters[_param].max, 0., 1.) : toNormalized(_param, value);
}
- (void)sync {
    const double value = [_host valueFor:_param];
    const double pos = [self positionFor:value];
    if (!interacting_ && (cont_ < 0 || !self.stepped ||
        std::round(cont_*parameters[_param].max) != std::round(pos*parameters[_param].max))) cont_ = pos;
    NSString* text = valueText(_param, value);
    if (pos != pos_ || ![_field.stringValue isEqualToString:text]) {
        pos_ = pos;
        if (!_field.currentEditor) _field.stringValue = text;
        self.needsDisplay = YES;
    }
    self.toolTip = _field.toolTip = [NSString stringWithFormat:@"%@. Drag, scroll, or type a value. Shift-drag for fine control; double-click resets.", _title];
    [_field setAccessibilityLabel:[NSString stringWithFormat:@"%@ value", _title]];
}
- (void)applyPosition:(double)p {
    cont_ = std::clamp(p, 0., 1.);
    const double value = self.stepped ? std::round(cont_*parameters[_param].max) : fromNormalized(_param, cont_);
    if (value != [_host valueFor:_param]) [_host set:_param value:value];
    [self sync];
}
- (void)mouseDown:(NSEvent*)event {
    if (event.clickCount >= 2) { [_host commit:_param value:parameters[_param].initial]; [self sync]; return; }
    [self.window makeFirstResponder:nil];
    interacting_ = YES; lastY_ = event.locationInWindow.y;
    if (cont_ < 0) cont_ = pos_;
    [_host begin:_param];
}
- (void)mouseDragged:(NSEvent*)event {
    if (!interacting_) return;
    const double y = event.locationInWindow.y;
    // Continuous knobs cross their range in 190 px (1000 px with Shift); stepped ones take 14 px per step.
    const double perPixel = self.stepped ? 1./(parameters[_param].max*14)
        : (event.modifierFlags & NSEventModifierFlagShift) ? 1./1000 : 1./190;
    [self applyPosition:cont_+(y-lastY_)*perPixel];
    lastY_ = y;
}
- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (!interacting_) return;
    interacting_ = NO; [_host end:_param]; [self sync];
}
- (void)scrollWheel:(NSEvent*)event {
    const double delta = event.scrollingDeltaY*(event.hasPreciseScrollingDeltas ? .004 : .02);
    if (delta == 0) return;
    if (cont_ < 0) cont_ = pos_;
    [_host begin:_param]; [self applyPosition:cont_+delta*(self.stepped ? 2 : 1)]; [_host end:_param];
}
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilitySliderRole; }
- (NSString*)accessibilityLabel { return _title; }
- (id)accessibilityValue { return valueText(_param, [_host valueFor:_param]); }
- (BOOL)accessibilityPerformIncrement { cont_ = pos_; [_host begin:_param]; [self applyPosition:pos_+(self.stepped ? 1./parameters[_param].max : .05)]; [_host end:_param]; return YES; }
- (BOOL)accessibilityPerformDecrement { cont_ = pos_; [_host begin:_param]; [self applyPosition:pos_-(self.stepped ? 1./parameters[_param].max : .05)]; [_host end:_param]; return YES; }
- (void)typed:(NSTextField*)sender {
    double value;
    if (parseValue(_param, sender.stringValue, value)) [_host commit:_param value:value];
    [self.window makeFirstResponder:nil]; [self sync];
}
- (void)controlTextDidEndEditing:(NSNotification*)notification {
    if ([notification.userInfo[@"NSTextMovement"] integerValue] != NSReturnTextMovement) [self typed:_field];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = self.bounds;
    drawText(_title, NSMakeRect(_labelShift, b.size.height-15, b.size.width, 12), 10, color(kDim), NSFontWeightSemibold, 1.6, NSTextAlignmentCenter);
    const NSPoint c = NSMakePoint(b.size.width/2, b.size.height-52);
    const CGFloat radius = 29;
    auto* track = [NSBezierPath bezierPath];
    [track appendBezierPathWithArcWithCenter:c radius:radius startAngle:225 endAngle:-45 clockwise:YES];
    track.lineWidth = 4; track.lineCapStyle = NSLineCapStyleRound;
    [color(kTrack) setStroke]; [track stroke];
    if (pos_ > .004) {
        auto* arc = [NSBezierPath bezierPath];
        [arc appendBezierPathWithArcWithCenter:c radius:radius startAngle:225 endAngle:225-270*pos_ clockwise:YES];
        arc.lineWidth = 4; arc.lineCapStyle = NSLineCapStyleRound;
        [NSGraphicsContext saveGraphicsState];
        auto* glow = [[NSShadow alloc] init];
        glow.shadowColor = [_tint colorWithAlphaComponent:.55]; glow.shadowBlurRadius = 9; glow.shadowOffset = NSZeroSize;
        [glow set]; [_tint setStroke]; [arc stroke];
        [NSGraphicsContext restoreGraphicsState];
    }
    const CGFloat body = radius-8;
    auto* disc = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(c.x-body, c.y-body, 2*body, 2*body)];
    [[[NSGradient alloc] initWithStartingColor:color(0x2C3440) endingColor:color(0x151A21)] drawInBezierPath:disc angle:-90];
    [color(0x3A4352) setStroke]; disc.lineWidth = 1; [disc stroke];
    const double angle = (225-270*pos_)*M_PI/180;
    auto* tick = [NSBezierPath bezierPath];
    [tick moveToPoint:NSMakePoint(c.x+std::cos(angle)*5, c.y+std::sin(angle)*5)];
    [tick lineToPoint:NSMakePoint(c.x+std::cos(angle)*(body-3), c.y+std::sin(angle)*(body-3))];
    tick.lineWidth = 2.5; tick.lineCapStyle = NSLineCapStyleRound;
    [color(0xF4F1EA) setStroke]; [tick stroke];
}
@end

// ---------------------------------------------------------------- toggle
@interface GDToggle : NSView
@property (weak) id<GDHost> host;
@property uint32_t param;
@property NSString* title;
@property NSColor* tint;
@property BOOL big;
@property (nonatomic) BOOL on;
@end
@implementation GDToggle
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)setOn:(BOOL)on { if (_on != on) { _on = on; self.needsDisplay = YES; } }
- (void)mouseDown:(NSEvent*)event { (void)event; [self.window makeFirstResponder:nil]; [_host commit:_param value:_on ? 0 : 1]; self.on = !_on; }
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityCheckBoxRole; }
- (NSString*)accessibilityLabel { return _title; }
- (id)accessibilityValue { return @(_on); }
- (BOOL)accessibilityPerformPress { [_host commit:_param value:_on ? 0 : 1]; self.on = !_on; return YES; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect r = NSInsetRect(self.bounds, 3, 3);
    auto* path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:_big ? 10 : 6 yRadius:_big ? 10 : 6];
    [NSGraphicsContext saveGraphicsState];
    if (_on) {
        auto* glow = [[NSShadow alloc] init];
        glow.shadowColor = [_tint colorWithAlphaComponent:_big ? .5 : .35]; glow.shadowBlurRadius = _big ? 14 : 7; glow.shadowOffset = NSZeroSize;
        [glow set];
    }
    [(_on ? [_tint colorWithAlphaComponent:.16] : color(kPanel)) setFill]; [path fill];
    [NSGraphicsContext restoreGraphicsState];
    [(_on ? _tint : color(kLine)) setStroke]; path.lineWidth = 1; [path stroke];
    const CGFloat size = _big ? 13 : 9, height = size+4;
    drawText(_title, NSMakeRect(r.origin.x, r.origin.y+(r.size.height-height)/2+(_big ? 0 : .5), r.size.width, height), size,
             _on ? _tint : color(kDim), NSFontWeightBold, _big ? 2.4 : 1.2, NSTextAlignmentCenter);
}
@end

// ---------------------------------------------------------------- XY pad
@interface GDPad : NSView
@property (weak) id<GDHost> host;
@property NSColor* tint;
@end
@implementation GDPad {
    BOOL dragging_;
}
- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (NSRect)field { return NSMakeRect(20, 30, self.bounds.size.width-40, self.bounds.size.height-50); }
- (void)updateFrom:(NSEvent*)event {
    const NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    const NSRect f = self.field;
    double interval = std::clamp((p.x-f.origin.x)/f.size.width, 0., 1.)*200;
    const double warp = std::clamp((p.y-f.origin.y)/f.size.height, 0., 1.)*100;
    if (!(event.modifierFlags & NSEventModifierFlagShift) && std::abs(interval-100) <= 3) interval = 100;
    [_host set:Interval value:interval]; [_host set:Warp value:warp];
    self.needsDisplay = YES;
}
- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:nil];
    if (event.clickCount >= 2) {
        [_host begin:Interval]; [_host begin:Warp];
        [_host set:Interval value:parameters[Interval].initial]; [_host set:Warp value:parameters[Warp].initial];
        [_host end:Interval]; [_host end:Warp]; self.needsDisplay = YES; return;
    }
    dragging_ = YES; [_host begin:Interval]; [_host begin:Warp]; [self updateFrom:event];
}
- (void)mouseDragged:(NSEvent*)event { if (dragging_) [self updateFrom:event]; }
- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (!dragging_) return;
    dragging_ = NO; [_host end:Interval]; [_host end:Warp]; self.needsDisplay = YES;
}
- (BOOL)isAccessibilityElement { return YES; }
- (NSAccessibilityRole)accessibilityRole { return NSAccessibilityGroupRole; }
- (NSString*)accessibilityLabel { return @"Interval and Warp pad"; }
- (id)accessibilityValue {
    return [NSString stringWithFormat:@"Interval %.0f percent, Warp %.0f percent", [_host valueFor:Interval], [_host valueFor:Warp]];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = NSInsetRect(self.bounds, .5, .5), f = self.field;
    const double interval = [_host valueFor:Interval], warp = [_host valueFor:Warp];
    auto* frame = [NSBezierPath bezierPathWithRoundedRect:b xRadius:14 yRadius:14];
    [[[NSGradient alloc] initWithStartingColor:color(0x1A2029) endingColor:color(0x12171E)] drawInBezierPath:frame angle:-90];
    [NSGraphicsContext saveGraphicsState];
    [frame addClip];
    const NSPoint puck = NSMakePoint(f.origin.x+interval/200*f.size.width, f.origin.y+warp/100*f.size.height);
    [[[NSGradient alloc] initWithColors:@[[_tint colorWithAlphaComponent:.05+.17*warp/100], [_tint colorWithAlphaComponent:0]]]
        drawFromCenter:puck radius:0 toCenter:puck radius:150 options:0];
    // Grid, with the 100% detent marked.
    for (int i = 0; i <= 8; ++i) {
        const CGFloat x = f.origin.x+f.size.width*i/8, y = f.origin.y+f.size.height*i/8;
        auto* v = [NSBezierPath bezierPath]; [v moveToPoint:NSMakePoint(x, f.origin.y)]; [v lineToPoint:NSMakePoint(x, NSMaxY(f))];
        auto* h = [NSBezierPath bezierPath]; [h moveToPoint:NSMakePoint(f.origin.x, y)]; [h lineToPoint:NSMakePoint(NSMaxX(f), y)];
        const bool major = i%4 == 0;
        [color(0xFFFFFF, major ? .075 : .035) setStroke]; v.lineWidth = h.lineWidth = 1; [v stroke]; [h stroke];
    }
    auto* detent = [NSBezierPath bezierPath];
    [detent moveToPoint:NSMakePoint(NSMidX(f), f.origin.y)]; [detent lineToPoint:NSMakePoint(NSMidX(f), NSMaxY(f))];
    const CGFloat dash[] {3, 4}; [detent setLineDash:dash count:2 phase:0];
    [[_tint colorWithAlphaComponent:.35] setStroke]; detent.lineWidth = 1; [detent stroke];
    // Crosshair from the puck to the axes.
    auto* cross = [NSBezierPath bezierPath];
    [cross moveToPoint:NSMakePoint(puck.x, f.origin.y)]; [cross lineToPoint:puck];
    [cross moveToPoint:NSMakePoint(f.origin.x, puck.y)]; [cross lineToPoint:puck];
    [[_tint colorWithAlphaComponent:.3] setStroke]; cross.lineWidth = 1; [cross stroke];
    [NSGraphicsContext restoreGraphicsState];
    // Axis tick labels.
    drawText(@"0", NSMakeRect(f.origin.x-10, 8, 20, 12), 8, color(kFaint), NSFontWeightMedium, .6, NSTextAlignmentCenter);
    drawText(@"100", NSMakeRect(NSMidX(f)-16, 8, 32, 12), 8, color(kDim), NSFontWeightSemibold, .6, NSTextAlignmentCenter);
    drawText(@"200", NSMakeRect(NSMaxX(f)-16, 8, 32, 12), 8, color(kFaint), NSFontWeightMedium, .6, NSTextAlignmentCenter);
    drawText(@"100", NSMakeRect(2, NSMaxY(f)-5, 16, 12), 8, color(kFaint), NSFontWeightMedium, .6, NSTextAlignmentRight);
    // Puck.
    const CGFloat glowRadius = dragging_ ? 22 : 18;
    [NSGraphicsContext saveGraphicsState];
    auto* shadow = [[NSShadow alloc] init];
    shadow.shadowColor = [_tint colorWithAlphaComponent:.9]; shadow.shadowBlurRadius = 18; shadow.shadowOffset = NSZeroSize;
    [shadow set];
    auto* ring = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(puck.x-glowRadius/2, puck.y-glowRadius/2, glowRadius, glowRadius)];
    [_tint setStroke]; ring.lineWidth = 1.5; [ring stroke];
    [NSGraphicsContext restoreGraphicsState];
    auto* core = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(puck.x-4.5, puck.y-4.5, 9, 9)];
    [color(0xFAF6EE) setFill]; [core fill];
    [color(kLine, .9) setStroke]; frame.lineWidth = 1; [frame stroke];
}
@end

// ---------------------------------------------------------------- meter strip
@interface GDMeters : NSView
@property double input, output, loop;
@property NSString* presetName;
@property NSString* stack;
@end
@implementation GDMeters
- (BOOL)isFlipped { return YES; }
- (void)bar:(double)value row:(int)row title:(NSString*)title tint:(NSColor*)tint glow:(BOOL)glow {
    const CGFloat y = 17+row*22, x = 56, w = self.bounds.size.width-x-282;
    drawText(title, NSMakeRect(16, y-2, 40, 12), 8.5, color(kDim), NSFontWeightSemibold, 1.4, NSTextAlignmentLeft);
    auto* track = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, y, w, 8) xRadius:4 yRadius:4];
    [color(kTrack) setFill]; [track fill];
    const CGFloat filled = std::max<CGFloat>(8, w*std::clamp(value, 0., 1.));
    if (value <= .003) return;
    [NSGraphicsContext saveGraphicsState];
    if (glow) {
        auto* shadow = [[NSShadow alloc] init];
        shadow.shadowColor = [tint colorWithAlphaComponent:.8]; shadow.shadowBlurRadius = 10; shadow.shadowOffset = NSZeroSize;
        [shadow set];
    }
    auto* fill = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x, y, filled, 8) xRadius:4 yRadius:4];
    [[[NSGradient alloc] initWithStartingColor:[tint colorWithAlphaComponent:.55] endingColor:tint] drawInBezierPath:fill angle:0];
    [NSGraphicsContext restoreGraphicsState];
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    const NSRect b = NSInsetRect(self.bounds, .5, .5);
    auto* panel = [NSBezierPath bezierPathWithRoundedRect:b xRadius:12 yRadius:12];
    [color(kPanel) setFill]; [panel fill]; [color(kLine) setStroke]; panel.lineWidth = 1; [panel stroke];
    [self bar:_input row:0 title:@"IN" tint:color(kSky) glow:NO];
    [self bar:_output row:1 title:@"OUT" tint:color(kTeal) glow:NO];
    [self bar:_loop row:2 title:@"LOOP" tint:color(kAmber) glow:YES];
    const CGFloat x = b.size.width-250;
    auto* divider = [NSBezierPath bezierPath];
    [divider moveToPoint:NSMakePoint(x-12, 12)]; [divider lineToPoint:NSMakePoint(x-12, b.size.height-12)];
    [color(kLine) setStroke]; [divider stroke];
    drawText(@"VOICES", NSMakeRect(x, 13, 120, 12), 8.5, color(kDim), NSFontWeightSemibold, 1.6, NSTextAlignmentLeft);
    drawText(_presetName ? _presetName : @"", NSMakeRect(x, 28, 236, 20), 15, color(kText), NSFontWeightSemibold, .2, NSTextAlignmentLeft);
    auto* mono = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium];
    [(_stack ? _stack : @"") drawInRect:NSMakeRect(x, 52, 236, 16) withAttributes:@{NSFontAttributeName: mono, NSForegroundColorAttributeName: color(kAmber)}];
}
@end

// ---------------------------------------------------------------- main view
@interface GDView : NSView <GDHost> {
    EditorCallbacks callbacks_;
    double shown_[ParamCount];
    CFAbsoluteTime held_[ParamCount];
    NSMutableArray<GDKnob*>* knobs_;
    GDKnob* timeKnob_;
    NSSegmentedControl* character_;
    NSPopUpButton* voices_;
    NSPopUpButton* warpType_;
    NSMutableDictionary<NSNumber*,GDToggle*>* toggles_;
    GDPad* pad_;
    GDMeters* meters_;
    NSTimer* timer_;
}
- (instancetype)initWithCallbacks:(EditorCallbacks)callbacks;
- (void)refresh;
- (void)start;
- (void)stop;
@end

@implementation GDView
- (BOOL)isFlipped { return YES; }
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    // Keep text shortcuts working even when the host has no native Edit menu.
    if ((event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask) == NSEventModifierFlagCommand) {
        auto* responder = self.window.firstResponder;
        BOOL ours = NO;
        for (GDKnob* knob in knobs_) if (knob.field.currentEditor == responder) { ours = YES; break; }
        if (ours && [responder isKindOfClass:NSTextView.class]) {
            auto* editor = (NSTextView*)responder;
            NSString* key = event.charactersIgnoringModifiers;
            if ([key isEqualToString:@"a"]) { [editor selectAll:self]; return YES; }
            if ([key isEqualToString:@"c"]) { [editor copy:self]; return YES; }
            if ([key isEqualToString:@"v"]) { [editor paste:self]; return YES; }
            if ([key isEqualToString:@"x"]) { [editor cut:self]; return YES; }
        }
    }
    return [super performKeyEquivalent:event];
}

// GDHost. Values are cached locally during and just after an edit so controls
// don't snap back while the host drains the UI queue.
- (double)valueFor:(uint32_t)pid { return pid < ParamCount ? shown_[pid] : 0; }
- (void)begin:(uint32_t)pid { held_[pid] = DBL_MAX; callbacks_.edit(callbacks_.context, pid, 0, 1); }
- (void)set:(uint32_t)pid value:(double)value {
    value = sanitize(pid, value);
    shown_[pid] = value;
    callbacks_.edit(callbacks_.context, pid, value, 0);
}
- (void)end:(uint32_t)pid { held_[pid] = CFAbsoluteTimeGetCurrent()+.25; callbacks_.edit(callbacks_.context, pid, 0, 2); }
- (void)commit:(uint32_t)pid value:(double)value { [self begin:pid]; [self set:pid value:value]; [self end:pid]; }

- (GDKnob*)knob:(uint32_t)pid title:(NSString*)title tint:(NSColor*)tint at:(NSPoint)origin {
    auto* knob = [[GDKnob alloc] initWithFrame:NSMakeRect(origin.x, origin.y, 100, 116)];
    knob.host = self; knob.param = pid; knob.title = title; knob.tint = tint;
    [self addSubview:knob]; [knobs_ addObject:knob];
    return knob;
}
- (GDToggle*)toggle:(uint32_t)pid title:(NSString*)title tint:(NSColor*)tint big:(BOOL)big frame:(NSRect)frame {
    auto* toggle = [[GDToggle alloc] initWithFrame:frame];
    toggle.host = self; toggle.param = pid; toggle.title = title; toggle.tint = tint; toggle.big = big;
    [self addSubview:toggle]; toggles_[@(pid)] = toggle;
    return toggle;
}
- (NSPopUpButton*)popup:(uint32_t)pid count:(int)count frame:(NSRect)frame label:(NSString*)label {
    auto* popup = [[NSPopUpButton alloc] initWithFrame:frame pullsDown:NO];
    for (int i = 0; i < count; ++i) [popup addItemWithTitle:[NSString stringWithUTF8String:paramLabel(pid, i)]];
    popup.target = self; popup.action = @selector(chosen:); popup.tag = pid;
    popup.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    [popup setAccessibilityLabel:label];
    [self addSubview:popup];
    return popup;
}

- (instancetype)initWithCallbacks:(EditorCallbacks)callbacks {
    self = [super initWithFrame:NSMakeRect(0, 0, Editor::width, Editor::height)];
    if (!self) return nil;
    callbacks_ = callbacks;
    self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    knobs_ = [NSMutableArray array]; toggles_ = [NSMutableDictionary dictionary];
    for (uint32_t i = 0; i < ParamCount; ++i) { shown_[i] = parameters[i].initial; held_[i] = 0; }

    // Header.
    character_ = [NSSegmentedControl segmentedControlWithLabels:@[@"Cloud", @"Spectral"]
        trackingMode:NSSegmentSwitchTrackingSelectOne target:self action:@selector(characterChanged:)];
    character_.frame = NSMakeRect(310, 26, 184, 28);
    character_.font = [NSFont systemFontOfSize:12 weight:NSFontWeightSemibold];
    character_.toolTip = @"Cloud: granular pitch shifter. Spectral: STFT engine with spectral warps.";
    [character_ setAccessibilityLabel:@"Character"];
    [self addSubview:character_];
    [self toggle:Freeze title:@"FREEZE" tint:color(kIce) big:YES frame:NSMakeRect(500, 18, 140, 44)].toolTip =
        @"Lock the loop: input stops, feedback holds. In Spectral the analysis frame is held too.";
    [self toggle:Bypass title:@"BYPASS" tint:color(kRose) big:YES frame:NSMakeRect(646, 18, 96, 44)];

    // Voices and warp type.
    voices_ = [self popup:Voices count:(int)voicePresets.size() frame:NSMakeRect(22, 114, 196, 26) label:@"Voices"];
    voices_.toolTip = @"Interval stack added to the loop. Shimmer sets how much of it you hear.";
    warpType_ = [self popup:WarpType count:(int)warpNames.size() frame:NSMakeRect(232, 114, 190, 26) label:@"Warp type"];
    warpType_.toolTip = @"Spectral only: what the Warp axis does to the spectrum.";

    // Knobs: two rows of four.
    const CGFloat x0 = 22, y0 = 152, dx = 100, dy = 128;
    [self knob:Shimmer title:@"SHIMMER" tint:color(kAmber) at:NSMakePoint(x0, y0)];
    [self knob:Bloom title:@"BLOOM" tint:color(kEmber) at:NSMakePoint(x0+dx, y0)];
    [self knob:Space title:@"SPACE" tint:color(kSky) at:NSMakePoint(x0+2*dx, y0)];
    timeKnob_ = [self knob:Time title:@"TIME" tint:color(kTeal) at:NSMakePoint(x0+3*dx, y0)];
    timeKnob_.labelShift = -17;
    [self toggle:Sync title:@"SYNC" tint:color(kTeal) big:NO frame:NSMakeRect(x0+3*dx+52, y0-1, 46, 22)].toolTip =
        @"Lock Time to the host tempo. The knob then selects a note division.";
    [self knob:Texture title:@"TEXTURE" tint:color(kViolet) at:NSMakePoint(x0, y0+dy)];
    [self knob:Tone title:@"TONE" tint:color(kViolet) at:NSMakePoint(x0+dx, y0+dy)];
    [self knob:Mix title:@"MIX" tint:color(kSteel) at:NSMakePoint(x0+2*dx, y0+dy)];
    [self knob:Output title:@"OUTPUT" tint:color(kSteel) at:NSMakePoint(x0+3*dx, y0+dy)];

    // XY pad.
    pad_ = [[GDPad alloc] initWithFrame:NSMakeRect(456, 114, 282, 270)];
    pad_.host = self; pad_.tint = color(kAmber);
    pad_.toolTip = @"X: Interval (snaps to 100%; hold Shift to disable). Y: Warp. Double-click to reset.";
    [self addSubview:pad_];

    meters_ = [[GDMeters alloc] initWithFrame:NSMakeRect(22, 420, 716, 80)];
    [self addSubview:meters_];

    [self refresh];
    return self;
}
- (void)characterChanged:(NSSegmentedControl*)sender { [self commit:Character value:sender.selectedSegment]; [self refresh]; }
- (void)chosen:(NSPopUpButton*)sender { [self commit:(uint32_t)sender.tag value:sender.indexOfSelectedItem]; [self refresh]; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [color(kBackground) setFill]; NSRectFill(self.bounds);
    [[[NSGradient alloc] initWithStartingColor:color(0x1A212C) endingColor:color(kBackground)]
        drawInRect:NSMakeRect(0, 0, self.bounds.size.width, 150) angle:90];
    drawText(@"GRANULAR DELAY", NSMakeRect(22, 15, 290, 34), 25, color(0xF2F2ED), NSFontWeightHeavy, 3, NSTextAlignmentLeft);
    drawText(@"SHIMMER  ·  SPECTRAL", NSMakeRect(24, 49, 260, 14), 9, color(kAmber), NSFontWeightBold, 2.6, NSTextAlignmentLeft);
    auto* rule = [NSBezierPath bezierPath];
    [rule moveToPoint:NSMakePoint(22, 78.5)]; [rule lineToPoint:NSMakePoint(self.bounds.size.width-22, 78.5)];
    [color(kLine) setStroke]; rule.lineWidth = 1; [rule stroke];
    const bool spectral = shown_[Character] > .5;
    drawText(@"VOICES", NSMakeRect(24, 98, 120, 12), 9, color(kDim), NSFontWeightSemibold, 1.6, NSTextAlignmentLeft);
    drawText(@"WARP TYPE", NSMakeRect(234, 98, 120, 12), 9, color(spectral ? kDim : kFaint), NSFontWeightSemibold, 1.6, NSTextAlignmentLeft);
    const NSRect pad = pad_.frame;
    drawText([NSString stringWithFormat:@"INTERVAL   %.0f %%", shown_[Interval]],
             NSMakeRect(pad.origin.x, NSMaxY(pad)+7, pad.size.width, 12), 9, color(kDim), NSFontWeightSemibold, 1.6, NSTextAlignmentCenter);
    // Vertical axis caption, read bottom-to-top along the pad's left edge.
    [NSGraphicsContext saveGraphicsState];
    auto* transform = [NSAffineTransform transform];
    [transform translateXBy:pad.origin.x-9 yBy:NSMidY(pad)];
    [transform rotateByDegrees:-90];
    [transform concat];
    drawText([NSString stringWithFormat:@"%@   %.0f %%", spectral ? @"WARP" : @"WILD", shown_[Warp]],
             NSMakeRect(-100, -6, 200, 12), 9, color(kDim), NSFontWeightSemibold, 1.6, NSTextAlignmentCenter);
    [NSGraphicsContext restoreGraphicsState];
}
- (void)refresh {
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    for (uint32_t i = 0; i < ParamCount; ++i)
        if (now >= held_[i]) shown_[i] = sanitize(i, callbacks_.get(callbacks_.context, i));
    const bool spectral = shown_[Character] > .5, sync = shown_[Sync] > .5;
    if (character_.selectedSegment != (spectral ? 1 : 0)) { character_.selectedSegment = spectral ? 1 : 0; self.needsDisplay = YES; }
    if (voices_.indexOfSelectedItem != (NSInteger)shown_[Voices]) [voices_ selectItemAtIndex:(NSInteger)shown_[Voices]];
    if (warpType_.indexOfSelectedItem != (NSInteger)shown_[WarpType]) [warpType_ selectItemAtIndex:(NSInteger)shown_[WarpType]];
    warpType_.enabled = spectral; warpType_.alphaValue = spectral ? 1 : .4;
    timeKnob_.param = sync ? Division : Time;
    for (GDKnob* knob in knobs_) [knob sync];
    for (NSNumber* pid in toggles_) toggles_[pid].on = shown_[pid.unsignedIntValue] > .5;
    pad_.tint = color(spectral ? kIce : kAmber); pad_.needsDisplay = YES;
    const auto read = [&](unsigned index) { return callbacks_.meter ? std::sqrt(std::clamp(callbacks_.meter(callbacks_.context, index), 0.f, 1.f)) : 0.f; };
    const auto follow = [](double shown, double raw) { return raw > shown ? raw : shown*.86+raw*.14; };
    meters_.input = follow(meters_.input, read(0)); meters_.output = follow(meters_.output, read(1)); meters_.loop = follow(meters_.loop, read(2));
    const auto preset = static_cast<size_t>(std::clamp(shown_[Voices], 0., double(voicePresets.size()-1)));
    meters_.presetName = [NSString stringWithUTF8String:voicePresets[preset].name];
    meters_.stack = stackText(shown_[Interval], preset);
    meters_.needsDisplay = YES;
    [self setNeedsDisplayInRect:NSMakeRect(0, 90, self.bounds.size.width, 320)];
}
- (void)start {
    if (timer_) return;
    __weak GDView* weak = self;
    timer_ = [NSTimer timerWithTimeInterval:1./30 repeats:YES block:^(NSTimer*) { [weak refresh]; }];
    [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
}
- (void)stop { [timer_ invalidate]; timer_ = nil; }
- (void)dealloc { [timer_ invalidate]; }
@end

namespace granular {
class CocoaEditor final : public Editor {
public:
    explicit CocoaEditor(EditorCallbacks callbacks) { view_ = [[GDView alloc] initWithCallbacks:callbacks]; }
    ~CocoaEditor() override { [view_ stop]; [view_ removeFromSuperview]; view_ = nil; }
    bool setParent(void* parent) override {
        auto* host = (__bridge NSView*)parent;
        if (!host || !view_) return false;
        [view_ removeFromSuperview]; [host addSubview:view_]; return true;
    }
    void show(bool visible) override { view_.hidden = !visible; if (visible) [view_ start]; else [view_ stop]; }
    void* nativeView() const override { return (__bridge void*)view_; }
private:
    GDView* __strong view_;
};
Editor* Editor::create(EditorCallbacks callbacks) { return new CocoaEditor(callbacks); }
} // namespace granular
