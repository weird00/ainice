#ifndef HA_WEBHOOK_JSON_H
#define HA_WEBHOOK_JSON_H

/* 板端 sysroot 里 libcjson 的头文件布局随打包方式不同，两种常见位置都兼容。 */
#if defined(__has_include)
#  if __has_include(<cjson/cJSON.h>)
#    include <cjson/cJSON.h>
#  else
#    include <cJSON.h>
#  endif
#else
#  include <cjson/cJSON.h>
#endif

#endif
