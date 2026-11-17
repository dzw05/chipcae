#include <chipcae/jobs/v1/jobs.pb.h>
#include <chipcae/run/v1/run.pb.h>
#include <google/protobuf/stubs/common.h>
int main() {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    chipcae::run::v1::RunEvent event;
    event.set_seq(42);
    chipcae::run::v1::RunEvent copy;
    chipcae::jobs::v1::JobRef job;
    job.set_run_id("desktop-smoke");
    return copy.ParseFromString(event.SerializeAsString()) && copy.seq() == 42 &&
                   job.run_id() == "desktop-smoke"
               ? 0
               : 1;
}
