#ifndef _HF_NETDRIVER_NETDRIVER_H
#define _HF_NETDRIVER_NETDRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <stdint.h>

/*
 * Flags which will be passed to the original program running in a separate thread should go into
 * server_argc/server_argv
 */
int HonggfuzzNetDriverArgsForServer(int argc, char **argv, int *server_argc, char ***server_argv);
/*
 * TCP port that the fuzzed data inputs will be sent to
 */
uint16_t HonggfuzzNetDriverPort(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* ifndef _HF_NETDRIVER_NETDRIVER_H_ */