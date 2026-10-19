#include <juce_gui_extra/juce_gui_extra.h>
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <functional>
#include <map>
#define NovoxDropHandler NOVOX_DROP_HANDLER

@interface NovoxDropHandler : NSObject {
@public
  std::function<void(juce::String)> file;
  std::function<void(juce::String)> error;
  bool active;
}
@end
@implementation NovoxDropHandler
@end
static char dropKey;
static NovoxDropHandler* handler(id view) {return (NovoxDropHandler*)objc_getAssociatedObject(view,&dropKey);}
static bool readDrop(NSPasteboard* pb, NovoxDropHandler* h) {
  if(!h || !h->active) return false;
  NSArray* promises=[pb readObjectsForClasses:@[[NSFilePromiseReceiver class]] options:nil];
  if(promises.count>1) {h->error("Drop one region at a time");return true;}
  if(promises.count) {
    auto folder=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("NovoxDrops").getChildFile(juce::Uuid().toString());
    if(!folder.createDirectory()) {h->error("Could not receive the dragged region");return true;}
    NSURL* destination=[NSURL fileURLWithPath:[NSString stringWithUTF8String:folder.getFullPathName().toRawUTF8()] isDirectory:YES];
    for(NSFilePromiseReceiver* receiver in promises) {
      [receiver receivePromisedFilesAtDestination:destination options:@{} operationQueue:[NSOperationQueue mainQueue] reader:^(NSURL* url,NSError* failure) {
        if(!h->active) {if(url.isFileURL) juce::File(juce::String::fromUTF8(url.path.UTF8String)).deleteFile();return;}
        if(failure) h->error("Logic could not provide a file for this region. Export the region as MIDI or audio and drop that file.");
        else {
          const juce::String path=juce::String::fromUTF8(url.path.UTF8String);
          h->file(path);
          juce::File(path).deleteFile();
        }
      }];
    }
    return true;
  }
  NSArray* urls=[pb readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
  if(urls.count>1) {h->error("Drop one file at a time");return true;}
  if(urls.count) {h->file(juce::String::fromUTF8(((NSURL*)urls[0]).path.UTF8String));return true;}
  NSArray* paths=[pb propertyListForType:NSFilenamesPboardType];
  if([paths isKindOfClass:[NSArray class]] && paths.count>1) {h->error("Drop one file at a time");return true;}
  if([paths isKindOfClass:[NSArray class]] && paths.count) {h->file(juce::String::fromUTF8(((NSString*)paths[0]).UTF8String));return true;}
  NSData* midi=[pb dataForType:@"public.midi-audio"];
  if(midi.length>=4 && std::memcmp(midi.bytes,"MThd",4)==0) {
    auto temp=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Novox-"+juce::Uuid().toString()+".mid");
    if(temp.replaceWithData(midi.bytes,midi.length)) {h->file(temp.getFullPathName());temp.deleteFile();return true;}
  }
  juce::Logger::writeToLog("Novox region drop types: "+juce::String::fromUTF8([pb.types componentsJoinedByString:@", "].UTF8String));
  h->error("This region did not provide an audio or MIDI file. Export the region, then drop the exported file.");return true;
}
// Add drag overrides to JUCE's existing per-binary subclass; never change a
// live WKWebView's isa. AppKit/WebKit own its responder identity and lifetime.
static std::map<Class,std::map<SEL,IMP>> originalDragMethods;
static IMP originalMethod(id self,SEL selector) {
  for(Class cls=object_getClass(self);cls;cls=class_getSuperclass(cls)) {
    auto found=originalDragMethods.find(cls);
    if(found!=originalDragMethods.end()) {auto method=found->second.find(selector);if(method!=found->second.end()) return method->second;}
  }
  return nullptr;
}
static NSDragOperation dragEntered(id self,SEL command,id<NSDraggingInfo> sender) {
  if(auto* h=handler(self)) if(h->active) return NSDragOperationCopy;
  auto original=originalMethod(self,command);return original ? ((NSDragOperation(*)(id,SEL,id))original)(self,command,sender) : NSDragOperationNone;
}
static BOOL prepareDrop(id self,SEL command,id<NSDraggingInfo> sender) {
  if(auto* h=handler(self)) if(h->active) return YES;
  auto original=originalMethod(self,command);return original ? ((BOOL(*)(id,SEL,id))original)(self,command,sender) : NO;
}
static BOOL performDrop(id self,SEL command,id<NSDraggingInfo> sender) {
  if(readDrop(sender.draggingPasteboard,handler(self))) return YES;
  auto original=originalMethod(self,command);return original ? ((BOOL(*)(id,SEL,id))original)(self,command,sender) : NO;
}
static WKWebView* findWebView(NSView* view,NSView* root,juce::Rectangle<int> wanted) {
  if([view isKindOfClass:[WKWebView class]]) {
    const NSRect bounds=[view convertRect:view.bounds toView:root];
    if(std::abs(bounds.origin.x-wanted.getX())<8 && std::abs(bounds.origin.y-wanted.getY())<8 &&
       std::abs(bounds.size.width-wanted.getWidth())<8 && std::abs(bounds.size.height-wanted.getHeight())<8) return (WKWebView*)view;
  }
  for(NSView* child in view.subviews) if(auto* found=findWebView(child,root,wanted)) return found;
  return nil;
}
void* installNovoxRegionDrop(juce::Component& browser,std::function<void(juce::String)> file,
                             std::function<void(juce::String)> error) {
  auto* peer=browser.getPeer();if(!peer) return nullptr;
  // Find this browser's WebKit view via its native component bounds.
  NSView* root=(NSView*)peer->getNativeHandle();
  WKWebView* web=findWebView(root,root,peer->getComponent().getLocalArea(&browser,browser.getLocalBounds()));if(!web) return nullptr;
  if(handler(web)) return nullptr;
  Class original=object_getClass(web);
  // Do not modify Apple's base WKWebView class or views owned by other libraries.
  while(original && !juce::String(class_getName(original)).startsWith("WebViewKeyEquivalentResponder_")) original=class_getSuperclass(original);
  if(!original) return nullptr;
  if(originalDragMethods.find(original)==originalDragMethods.end()) {
    auto install=[&](SEL selector,IMP replacement,const char* encoding) {
      originalDragMethods[original][selector]=class_getMethodImplementation(original,selector);
      if(!class_addMethod(original,selector,replacement,encoding)) class_replaceMethod(original,selector,replacement,encoding);
    };
    install(@selector(draggingEntered:),(IMP)dragEntered,"Q@:@");
    install(@selector(draggingUpdated:),(IMP)dragEntered,"Q@:@");
    install(@selector(prepareForDragOperation:),(IMP)prepareDrop,"B@:@");
    install(@selector(performDragOperation:),(IMP)performDrop,"B@:@");
  }
  auto* h=[NovoxDropHandler new];h->active=true;h->file=std::move(file);h->error=std::move(error);
  objc_setAssociatedObject(web,&dropKey,h,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  NSMutableArray* types=[NSMutableArray arrayWithArray:web.registeredDraggedTypes];
  [types addObjectsFromArray:[NSFilePromiseReceiver readableDraggedTypes]];
  [types addObjectsFromArray:@[NSPasteboardTypeFileURL,NSFilenamesPboardType,NSPasteboardTypeString,@"public.midi-audio"]];
  [web registerForDraggedTypes:types];
  return h;
}
void readNovoxRegionDrop(void* token) {readDrop([NSPasteboard pasteboardWithName:NSPasteboardNameDrag],(NovoxDropHandler*)token);}
void removeNovoxRegionDrop(void* token) {if(token) {auto* h=(NovoxDropHandler*)token;h->active=false;[h release];}}

#if NOVOX_DROP_TESTS
// Use a private pasteboard so tests never touch the user's clipboard or drag.
bool testNovoxRegionDrops() {
  NSPasteboard* pb=[NSPasteboard pasteboardWithUniqueName];
  auto* h=[NovoxDropHandler new];h->active=true;
  juce::String received,message;bool midiRead=false;
  h->file=[&](juce::String path) {received=path;juce::MemoryBlock bytes;juce::File(path).loadFileAsData(bytes);midiRead=bytes.getSize()==4 && std::memcmp(bytes.getData(),"MThd",4)==0;};
  h->error=[&](juce::String text) {message=text;};
  auto temp=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Novox-drop-test-"+juce::Uuid().toString()+".mid");
  temp.replaceWithData("MThd",4);
  NSURL* url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:temp.getFullPathName().toRawUTF8()]];
  [pb writeObjects:@[url]];
  bool ok=readDrop(pb,h) && received==temp.getFullPathName() && midiRead && message.isEmpty();
  [pb clearContents];received.clear();midiRead=false;
  [pb setData:[NSData dataWithBytes:"MThd" length:4] forType:@"public.midi-audio"];
  ok=ok && readDrop(pb,h) && midiRead && !juce::File(received).existsAsFile();
  [pb clearContents];received.clear();
  [pb setString:@"private region" forType:@"com.novox.test.private-region"];
  ok=ok && readDrop(pb,h) && received.isEmpty() && message.isNotEmpty();
  h->active=false;ok=ok && !readDrop(pb,h);
  temp.deleteFile();[pb releaseGlobally];[h release];return ok;
}
#endif
