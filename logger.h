#ifndef CHRONO_LOGGER_H
#define CHRONO_LOGGER_H

int logger_init(int log_sig_no, char* log_filename, int dump_sig_no,  void* (*get_dump_data)(), size_t dump_size);
void logger_destroy();
int logger_log(int level, const char* format, ...);

#endif
