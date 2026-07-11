#import "browser_view.h"

@implementation AntBrowserView

- (instancetype)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  if (!self) return nil;
  self.wantsLayer = YES;
  self.layer.backgroundColor = NSColor.blackColor.CGColor;
  return self;
}

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)event {
  return self.acceptsFirstMouseOption;
}

@end
