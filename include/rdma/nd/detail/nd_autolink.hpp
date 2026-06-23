#pragma once

#if defined(ASIO_SEPARATE_COMPILATION) && \
    !defined(ASIO_NO_DEFAULT_LINKED_LIBS) && defined(_MSC_VER)
#pragma comment(lib, "ndutil.lib")
#endif
