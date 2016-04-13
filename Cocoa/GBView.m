#import <OpenGL/gl.h>
#import "GBView.h"
#import "GBButtons.h"
#import "NSString+StringForKey.h"

@implementation GBView
{
    uint32_t *image_buffers[3];
    unsigned char current_buffer;
}

- (void) _init
{
    image_buffers[0] = malloc(160 * 144 * 4);
    image_buffers[1] = malloc(160 * 144 * 4);
    image_buffers[2] = malloc(160 * 144 * 4);
    _shouldBlendFrameWithPrevious = 1;
}

- (unsigned char) numberOfBuffers
{
    return _shouldBlendFrameWithPrevious? 3 : 2;
}

- (void)dealloc
{
    free(image_buffers[0]);
    free(image_buffers[1]);
    free(image_buffers[2]);
}
- (instancetype)initWithCoder:(NSCoder *)coder
{
    if (!(self = [super initWithCoder:coder]))
    {
        return self;
    }
    [self _init];
    return self;
}

- (instancetype)initWithFrame:(NSRect)frameRect
{
    if (!(self = [super initWithFrame:frameRect]))
    {
        return self;
    }
    [self _init];
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    double scale = self.window.backingScaleFactor;
    glRasterPos2d(-1, 1);
    glPixelZoom(self.bounds.size.width / 160 * scale, self.bounds.size.height / -144 * scale);
    glDrawPixels(160, 144, GL_ABGR_EXT, GL_UNSIGNED_BYTE, image_buffers[current_buffer]);
    if (_shouldBlendFrameWithPrevious) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        glBlendColor(1, 1, 1, 0.5);
        glDrawPixels(160, 144, GL_ABGR_EXT, GL_UNSIGNED_BYTE, image_buffers[(current_buffer + 2) % self.numberOfBuffers]);
        glDisable(GL_BLEND);
    }
    glFlush();
}


- (void) flip
{
    current_buffer = (current_buffer + 1) % self.numberOfBuffers;
    [self setNeedsDisplay:YES];
}

- (uint32_t *) pixels
{
    return image_buffers[(current_buffer + 1) % self.numberOfBuffers];
}

-(void)keyDown:(NSEvent *)theEvent
{
    bool handled = false;

    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    for (GBButton i = 0; i < GBButtonCount; i++) {
        if ([[defaults stringForKey:button_to_preference_name(i)] isEqualToString:theEvent.charactersIgnoringModifiers]) {
            handled = true;
            switch (i) {
                case GBTurbo:
                    _gb->turbo = true;
                    break;
                    
                default:
                    _gb->keys[i] = true;
                    break;
            }
        }
    }

    if (!handled) {
        [super keyDown:theEvent];
    }
}

-(void)keyUp:(NSEvent *)theEvent
{
    bool handled = false;

    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    for (GBButton i = 0; i < GBButtonCount; i++) {
        if ([[defaults stringForKey:button_to_preference_name(i)] isEqualToString:theEvent.charactersIgnoringModifiers]) {
            handled = true;
            switch (i) {
                case GBTurbo:
                    _gb->turbo = false;
                    break;

                default:
                    _gb->keys[i] = false;
                    break;
            }
        }
    }

    if (!handled) {
        [super keyUp:theEvent];
    }
}

-(void)reshape
{
    double scale = self.window.backingScaleFactor;
    glViewport(0, 0, self.bounds.size.width * scale, self.bounds.size.height * scale);
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}
@end
