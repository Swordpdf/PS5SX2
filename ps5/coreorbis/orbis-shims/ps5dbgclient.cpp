// Orbis: self-service allocation via the local ps5debug-NG server.
// Uses CMD_PROC_ALLOC_HINTED (0xBDAA000E) with a far-away hint address so the
// kernel-allocated memory cannot collide with the process's thread stacks.
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

extern "C" {
int sceNetInit(unsigned long long poolsize, int flags, unsigned long long ignore1, int flags2, unsigned long long ignore2);
}

static const unsigned char PACKET_MAGIC_B[4] = {0xCC, 0xBB, 0xAA, 0xFF}; // 0xFFAABBCC LE
static const unsigned char CMD_PROC_ALLOC_HINTED_B[4] = {0x0E, 0x00, 0xAA, 0xBD}; // 0xBDAA000E LE
static const unsigned char CMD_PROC_ARENA_B[4] = {0x24, 0xCC, 0xAA, 0xBD}; // 0xBDAACC24 LE

static bool recv_all(int fd, unsigned char* buf, int len)
{
    int off = 0;
    while (off < len)
    {
        int r = recv(fd, buf + off, len - off, 0);
        if (r <= 0)
            return false;
        off += r;
    }
    return true;
}

// Disables the server-side 16MB-segment arena so CMD_PROC_ALLOC_HINTED maps one
// contiguous region. Returns true on success.
static bool orbis_dbg_disable_arena(int fd)
{
    unsigned char req[12 + 4];
    std::memcpy(req + 0, PACKET_MAGIC_B, 4);
    std::memcpy(req + 4, CMD_PROC_ARENA_B, 4);
    const unsigned int datalen = 4;
    std::memcpy(req + 8, &datalen, 4);
    const unsigned int off = 0; // 0 = disable arena
    std::memcpy(req + 12, &off, 4);

    if (send(fd, req, sizeof(req), 0) != (int)sizeof(req))
        return false;

    unsigned char status_b[4];
    if (!recv_all(fd, status_b, 4))
        return false;
    unsigned int status = 0;
    std::memcpy(&status, status_b, 4);
    if (status != 0x80000000u)
        return false;

    unsigned char len_b[4] = {0};
    recv_all(fd, len_b, 4);
    unsigned int tlen = 0;
    std::memcpy(&tlen, len_b, 4);
    if (tlen > 0 && tlen <= 4096)
    {
        unsigned char tmp[4096];
        recv_all(fd, tmp, (int)tlen);
        printf("[dbgalloc] arena: %.*s\n", (int)tlen, tmp);
        fflush(stdout);
    }
    return true;
}

// Returns the kernel-allocated address in our process, or 0 on failure.
extern "C" unsigned long long orbis_self_alloc(unsigned long long len)
{
    static bool net_init_done = false;
    if (!net_init_done)
    {
        sceNetInit(0x100000, 0x42, (unsigned long long)-1, 1, 0);
        net_init_done = true;
    }

    int fd = socket(AF_INET /*2*/, SOCK_STREAM /*1*/, 0);
    if (fd < 0)
    {
        printf("[dbgalloc] socket failed errno=%d\n", errno);
        fflush(stdout);
        return 0;
    }

    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(744);
    sa.sin_addr.s_addr = htonl(0x7F000001); // 127.0.0.1
    if (connect(fd, (sockaddr*)&sa, sizeof(sa)) < 0)
    {
        printf("[dbgalloc] connect to local ps5debug failed (errno=%d)\n", errno);
        fflush(stdout);
        close(fd);
        return 0;
    }

    if (!orbis_dbg_disable_arena(fd))
        printf("[dbgalloc] arena disable failed (continuing)\n");

    // request: magic + cmd + datalen(16) + { pid u32, pad u32, hint u64, len u32 }
    int pid = (int)getpid();
    const unsigned long long hint = 0x200000000ULL; // 8GB - within the app's legal user area
    unsigned char req[12 + 16];
    std::memcpy(req + 0, PACKET_MAGIC_B, 4);
    std::memcpy(req + 4, CMD_PROC_ALLOC_HINTED_B, 4);
    const unsigned int datalen = 16;
    std::memcpy(req + 8, &datalen, 4);
    std::memcpy(req + 12, &pid, 4);
    const unsigned int pad = 0;
    std::memcpy(req + 16, &pad, 4);
    std::memcpy(req + 20, &hint, 8);
    const unsigned int len32 = (unsigned int)len;
    std::memcpy(req + 28, &len32, 4);

    const int total = (int)sizeof(req);
    int off = 0;
    while (off < total)
    {
        int w = send(fd, req + off, total - off, 0);
        if (w <= 0)
        {
            printf("[dbgalloc] send failed errno=%d\n", errno);
            fflush(stdout);
            close(fd);
            return 0;
        }
        off += w;
    }

    // response: 4-byte status, then 8-byte address (zeroed on failure)
    unsigned char resp[4];
    if (!recv_all(fd, resp, 4))
    {
        close(fd);
        return 0;
    }
    unsigned int status = 0;
    std::memcpy(&status, resp, 4);
    unsigned char addr_body[8] = {0};
    recv_all(fd, addr_body, 8);
    unsigned long long out = 0;
    std::memcpy(&out, addr_body, 8);
    printf("[dbgalloc] hinted alloc status=0x%08x addr=0x%llx\n", status, (unsigned long long)out);
    fflush(stdout);
    close(fd);
    if (status != 0x80000000u)
        return 0;
    return out;
}
