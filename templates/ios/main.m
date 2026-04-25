/**
 * ChocoLight Engine — iOS 模板入口
 * 初始化 Lumen Lua VM，从 Bundle 加载并执行 Lua 脚本
 * 支持加密脚本 (.enc) 自动解密
 */

#import <UIKit/UIKit.h>

// Lua 5.1 C API
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

// Script decryption support
#include "choco_crypt.h"

// 全局输出缓冲区
static NSMutableString *g_outputBuffer = nil;

// 自定义 print 函数：追加到输出缓冲区
static int l_ios_print(lua_State *L) {
    int n = lua_gettop(L);
    NSMutableString *line = [NSMutableString string];

    for (int i = 1; i <= n; i++) {
        const char *s = lua_tostring(L, i);
        if (i > 1) [line appendString:@"\t"];
        [line appendString:s ? [NSString stringWithUTF8String:s] : @"(null)"];
    }
    [line appendString:@"\n"];

    NSLog(@"[Lua] %@", line);
    if (g_outputBuffer) {
        [g_outputBuffer appendString:line];
    }
    return 0;
}

/**
 * 加载脚本数据：优先 .enc 加密版本，回退到明文
 */
static NSData *loadScriptData(const char *scriptName, BOOL *wasEncrypted) {
    NSString *name = [NSString stringWithUTF8String:scriptName];
    NSString *baseName = [name stringByDeletingPathExtension];
    NSString *ext = [name pathExtension];

    *wasEncrypted = NO;

    // 尝试加载 .enc 版本
    NSString *encName = [NSString stringWithFormat:@"%@.%@.enc", baseName, ext];
    NSString *encBase = [encName stringByDeletingPathExtension]; // "main.lua"
    NSString *encExt = @"enc";

    NSString *encPath = [[NSBundle mainBundle] pathForResource:
        [NSString stringWithFormat:@"%@.%@", baseName, ext] ofType:@"enc"];
    if (encPath) {
        NSData *encData = [NSData dataWithContentsOfFile:encPath];
        if (encData) {
            uint8_t *decrypted = NULL;
            size_t decLen = 0;
            if (choco_decrypt((const uint8_t *)[encData bytes], [encData length],
                              &decrypted, &decLen) == 0) {
                NSLog(@"[ChocoLight] Decrypted %@ (%zu bytes)", encPath, decLen);
                *wasEncrypted = YES;
                NSData *result = [NSData dataWithBytesNoCopy:decrypted length:decLen freeWhenDone:YES];
                return result;
            }
            NSLog(@"[ChocoLight] Decrypt failed for %@, trying plaintext", encPath);
        }
    }

    // 回退到明文
    NSString *path = [[NSBundle mainBundle] pathForResource:baseName ofType:ext];
    if (path) {
        return [NSData dataWithContentsOfFile:path];
    }
    return nil;
}

// 执行 Bundle 中的 Lua 脚本
static int runLuaScript(const char *scriptName, NSMutableString *output) {
    g_outputBuffer = output;

    lua_State *L = luaL_newstate();
    if (!L) return -1;
    luaL_openlibs(L);

    // 替换 print
    lua_pushcfunction(L, l_ios_print);
    lua_setglobal(L, "print");

    // 加载脚本（自动尝试 .enc 解密）
    BOOL wasEncrypted = NO;
    NSData *scriptData = loadScriptData(scriptName, &wasEncrypted);

    int status = -1;
    if (scriptData) {
        [output appendFormat:@"Loading %s%s...\n", scriptName,
            wasEncrypted ? " (encrypted)" : ""];
        status = luaL_loadbuffer(L, (const char *)[scriptData bytes],
                                [scriptData length], scriptName)
                 || lua_pcall(L, 0, 0, 0);
        if (status != 0) {
            const char *err = lua_tostring(L, -1);
            [output appendFormat:@"[ERROR] %s\n", err ? err : "unknown"];
            lua_pop(L, 1);
        }
    } else {
        [output appendFormat:@"[ERROR] Script not found: %s\n", scriptName];
    }

    lua_close(L);
    g_outputBuffer = nil;
    return status;
}

// ==================== UI ====================

@interface LightViewController : UIViewController
@property (nonatomic, strong) UITextView *outputView;
@end

@implementation LightViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithRed:0.1 green:0.1 blue:0.18 alpha:1.0];

    // 标题
    UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(0, 50, self.view.bounds.size.width, 40)];
    title.text = @"🍫 ChocoLight Engine";
    title.textColor = [UIColor colorWithRed:0.91 green:0.27 blue:0.38 alpha:1.0];
    title.font = [UIFont boldSystemFontOfSize:22];
    title.textAlignment = NSTextAlignmentCenter;
    [self.view addSubview:title];

    // 输出区域
    CGRect outputFrame = CGRectMake(16, 100,
        self.view.bounds.size.width - 32,
        self.view.bounds.size.height - 116);
    self.outputView = [[UITextView alloc] initWithFrame:outputFrame];
    self.outputView.backgroundColor = [UIColor colorWithRed:0.06 green:0.2 blue:0.38 alpha:1.0];
    self.outputView.textColor = [UIColor colorWithRed:0.0 green:1.0 blue:0.25 alpha:1.0];
    self.outputView.font = [UIFont fontWithName:@"Menlo" size:13];
    self.outputView.editable = NO;
    self.outputView.layer.cornerRadius = 8;
    self.outputView.text = @"Initializing Lua VM...\n";
    [self.view addSubview:self.outputView];

    // 后台执行 Lua
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        NSMutableString *output = [NSMutableString string];
        int status = runLuaScript("main.lua", output);
        [output appendFormat:@"\n--- Script finished (status=%d) ---\n", status];

        dispatch_async(dispatch_get_main_queue(), ^{
            self.outputView.text = output;
        });
    });
}

@end

// ==================== App Delegate ====================

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[LightViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
