#include "src/stirling/cassandra/cql_stitcher.h"

#include <deque>
#include <string>
#include <utility>

#include "src/common/base/base.h"
#include "src/stirling/cassandra/cass_types.h"

namespace pl {
namespace stirling {
namespace cass {

StatusOr<Record> ProcessEventFrame(Frame* resp_frame) {
  // Make a fake request to go along with the response.
  // - Use REGISTER op, since that was what set up the events in the first place.
  // - Use response timestamp, so any calculated latencies are reported as 0.
  Request req;
  req.op = ReqOp::kRegister;
  req.msg = "-";
  req.timestamp_ns = resp_frame->timestamp_ns;

  Response resp;
  resp.op = static_cast<RespOp>(resp_frame->hdr.opcode);
  resp.msg = std::move(resp_frame->msg);
  resp.timestamp_ns = resp_frame->timestamp_ns;

  return Record{req, resp};
}

StatusOr<Record> ProcessSimpleReqResp(Frame* req_frame, Frame* resp_frame) {
  Request req;
  req.op = static_cast<ReqOp>(req_frame->hdr.opcode);
  req.msg = req_frame->msg;
  req.timestamp_ns = req_frame->timestamp_ns;

  Response resp;
  resp.op = static_cast<RespOp>(resp_frame->hdr.opcode);
  resp.msg = std::move(resp_frame->msg);
  resp.timestamp_ns = resp_frame->timestamp_ns;

  return Record{req, resp};
}

StatusOr<Record> ProcessReqRespPair(Frame* req_frame, Frame* resp_frame) {
  ECHECK_LT(req_frame->timestamp_ns, resp_frame->timestamp_ns);

  switch (static_cast<ReqOp>(req_frame->hdr.opcode)) {
    case ReqOp::kStartup:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    case ReqOp::kAuthResponse:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    case ReqOp::kOptions:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    case ReqOp::kQuery:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    case ReqOp::kPrepare:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    case ReqOp::kExecute:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    case ReqOp::kBatch:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    case ReqOp::kRegister:
      return ProcessSimpleReqResp(req_frame, resp_frame);
    default:
      return error::Internal("Unhandled opcode $0", magic_enum::enum_name(req_frame->hdr.opcode));
  }
}

// Currently ProcessFrames() uses a response-led matching algorithm.
// For each response that is at the head of the deque, there should exist a previous request with
// the same stream. Find it, and consume both frames.
// TODO(oazizi): Does it make sense to sort to help the matching?
// Considerations:
//  - Request and response deques are likely (confirm?) to be mostly ordered.
//  - Stream values can be re-used, so sorting would have to consider times too.
//  - Stream values need not be in any sequential order.
std::vector<Record> ProcessFrames(std::deque<Frame>* req_frames, std::deque<Frame>* resp_frames) {
  std::vector<Record> entries;

  for (auto& resp_frame : *resp_frames) {
    bool found_match = false;

    // Event responses are special: they have no request.
    if (resp_frame.hdr.opcode == Opcode::kEvent) {
      StatusOr<Record> record_status = ProcessEventFrame(&resp_frame);
      if (record_status.ok()) {
        entries.push_back(record_status.ConsumeValueOrDie());
      } else {
        LOG(ERROR) << record_status.msg();
      }
      resp_frames->pop_front();
      continue;
    }

    // Search for matching req frame
    for (auto& req_frame : *req_frames) {
      if (resp_frame.hdr.stream == req_frame.hdr.stream) {
        VLOG(2) << absl::Substitute("req_op=$0 msg=$1", magic_enum::enum_name(req_frame.hdr.opcode),
                                    req_frame.msg);

        StatusOr<Record> record_status = ProcessReqRespPair(&req_frame, &resp_frame);
        if (record_status.ok()) {
          entries.push_back(record_status.ConsumeValueOrDie());
        } else {
          LOG(ERROR) << record_status.ToString();
        }

        // Found a match, so remove both request and response.
        // We don't remove request frames on the fly, however,
        // because it could otherwise cause unnecessary churn/copying in the deque.
        // This is due to the fact that responses can come out-of-order.
        // Just mark the request as consumed, and clean-up when they reach the head of the queue.
        // Note that responses are always head-processed, so they don't require this optimization.
        found_match = true;
        resp_frames->pop_front();
        req_frame.consumed = true;
        break;
      }
    }

    LOG_IF(ERROR, !found_match) << absl::Substitute(
        "Did not find a request matching the request. Stream = $0", resp_frame.hdr.stream);

    // Clean-up consumed frames at the head.
    // Do this inside the resp loop to aggressively clean-out req_frames whenever a frame consumed.
    // Should speed up the req_frames search for the next iteration.
    for (auto& req_frame : *req_frames) {
      if (!req_frame.consumed) {
        break;
      }
      req_frames->pop_front();
    }

    // TODO(oazizi): Consider removing requests that are too old, otherwise a lost response can mean
    // the are never processed. This would result in a memory leak until the more drastic connection
    // tracker clean-up mechanisms kick in.
  }

  return entries;
}

}  // namespace cass
}  // namespace stirling
}  // namespace pl
