set(app_sources "main.cpp")

idf_component_register(SRCS ${app_sources} REQUIRES epdiy)
