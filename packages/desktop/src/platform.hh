#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AntDesktopRegion {
  double x;
  double y;
  double width;
  double height;
  bool draggable;
} AntDesktopRegion;

void AntDesktop_OnWindowEvent(int64_t window, const char *type);
bool AntDesktop_OnWindowShouldClose(int64_t window);
void AntDesktop_OnWindowClosed(int64_t window);
void AntDesktop_OnMenuClick(int64_t item);
void AntDesktop_OnTerminate(void);

#ifdef __cplusplus
void AntDesktop_App_Initialize(void);
void AntDesktop_App_FinishLaunching(void);
void AntDesktop_App_Run(void);
void AntDesktop_App_Stop(void);
void AntDesktop_App_Terminate(void);
char *AntDesktop_App_Path(const char *name);
char *AntDesktop_App_Info(const char *key);
char *AntDesktop_CEF_Resolve(const char *version, const char *archive, const char *sha1, int64_t size);

bool AntDesktop_Window_Create(int64_t window, const char *options_json);
void *AntDesktop_Window_ContentView(int64_t window);
void AntDesktop_Window_Attach(int64_t window, void *browser_view);
void AntDesktop_Window_Detach(int64_t window);
void AntDesktop_Window_SetRegions(int64_t window, const AntDesktopRegion *regions, size_t count);
bool AntDesktop_Window_Perform(int64_t window, const char *operation, const char *text, bool flag);
bool AntDesktop_Window_Bounds(int64_t window, double *bounds);
void AntDesktop_Window_Close(int64_t window);
void AntDesktop_Menu_Set(const char *json);
}
#endif
