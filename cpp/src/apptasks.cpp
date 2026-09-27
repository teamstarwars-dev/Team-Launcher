// Portage de AppTasks.cs — registre thread-safe (mutex + atomiques).
// SANS ImGui. Windows uniquement, C++20.

#include "apptasks.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace tl::tasks {

namespace {

struct Entry {
    int id = 0;
    std::string title;
    std::string status;
    double progress = 0.0;
    State state = State::Running;
    std::atomic<bool> cancel{false};
    std::int64_t startedUnix = 0;
};

std::mutex g_m;
std::vector<std::shared_ptr<Entry>> g_items;
std::atomic<int> g_nextId{0};
std::atomic<std::uint64_t> g_version{0};

std::int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double clamp01(double p) {
    if (p < 0.0) return 0.0;
    if (p > 1.0) return 1.0;
    return p;
}

bool terminal(State s) {
    return s == State::Done || s == State::Failed || s == State::Cancelled;
}

std::shared_ptr<Entry> find_locked(int id) {
    for (const auto& e : g_items)
        if (e && e->id == id) return e;
    return nullptr;
}

} // namespace

int create(const std::string& title, const std::string& status) {
    auto e = std::make_shared<Entry>();
    e->id = g_nextId.fetch_add(1) + 1;
    e->title = title;
    e->status = status;
    e->startedUnix = now_unix();
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_items.push_back(e);
    }
    g_version.fetch_add(1);
    return e->id;
}

void update(int id, const std::string& status, double progress) {
    std::lock_guard<std::mutex> lk(g_m);
    auto e = find_locked(id);
    if (!e || terminal(e->state)) return;
    e->status = status;
    if (progress >= 0.0) e->progress = clamp01(progress);
    g_version.fetch_add(1);
}

bool finish(int id) {
    std::lock_guard<std::mutex> lk(g_m);
    auto e = find_locked(id);
    if (!e || terminal(e->state)) return e != nullptr && e->state == State::Done;
    if (e->cancel.load()) {
        e->state = State::Cancelled;
    } else {
        e->state = State::Done;
        if (e->progress <= 0.0) e->progress = 1.0;
    }
    g_version.fetch_add(1);
    return true;
}

bool fail(int id, const std::string& message) {
    std::lock_guard<std::mutex> lk(g_m);
    auto e = find_locked(id);
    if (!e || terminal(e->state)) return e != nullptr;
    if (!message.empty()) e->status = message;
    e->state = State::Failed;
    g_version.fetch_add(1);
    return true;
}

bool cancel(int id) {
    std::lock_guard<std::mutex> lk(g_m);
    auto e = find_locked(id);
    if (!e) return false;
    e->cancel.store(true);
    if (e->state == State::Running) e->state = State::Cancelled;
    g_version.fetch_add(1);
    return true;
}

bool cancelled(int id) {
    std::lock_guard<std::mutex> lk(g_m);
    auto e = find_locked(id);
    return e != nullptr && e->cancel.load();
}

std::vector<Info> snapshot() {
    std::vector<Info> out;
    std::lock_guard<std::mutex> lk(g_m);
    out.reserve(g_items.size());
    for (const auto& e : g_items) {
        if (!e) continue;
        Info i;
        i.id = e->id;
        i.title = e->title;
        i.status = e->status;
        i.progress = e->progress;
        i.state = e->state;
        i.cancelRequested = e->cancel.load();
        i.startedUnix = e->startedUnix;
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(),
              [](const Info& a, const Info& b) { return a.id < b.id; });
    return out;
}

std::size_t count() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_items.size();
}

std::size_t count_running() {
    std::lock_guard<std::mutex> lk(g_m);
    std::size_t n = 0;
    for (const auto& e : g_items)
        if (e && e->state == State::Running) ++n;
    return n;
}

void clear_finished() {
    std::lock_guard<std::mutex> lk(g_m);
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(),
                                 [](const std::shared_ptr<Entry>& e) {
                                     return !e || terminal(e->state);
                                 }),
                  g_items.end());
    g_version.fetch_add(1);
}

std::uint64_t version() { return g_version.load(); }

void reset_for_tests() {
    std::lock_guard<std::mutex> lk(g_m);
    g_items.clear();
    g_nextId.store(0);
    g_version.fetch_add(1);
}

std::future<void> run(
    std::string title,
    std::function<void(const std::atomic<bool>&,
                       const std::function<void(const std::string&, double)>&)>
        work,
    std::function<void(std::exception_ptr)> on_error) {
    const int id = create(title);
    return std::async(std::launch::async,
                      [id, work = std::move(work),
                       on_error = std::move(on_error)] {
                          std::shared_ptr<Entry> e;
                          {
                              std::lock_guard<std::mutex> lk(g_m);
                              e = find_locked(id);
                          }
                          if (!e) return;
                          auto set_status =
                              [id](const std::string& s, double p) {
                                  update(id, s, p);
                              };
                          try {
                              work(e->cancel, set_status);
                              // Si annule pendant le travail : Cancelled, sinon Done.
                              std::lock_guard<std::mutex> lk(g_m);
                              auto self = find_locked(id);
                              if (self && !terminal(self->state)) {
                                  if (self->cancel.load())
                                      self->state = State::Cancelled;
                                  else {
                                      self->state = State::Done;
                                      if (self->progress <= 0.0)
                                          self->progress = 1.0;
                                  }
                                  g_version.fetch_add(1);
                              }
                          } catch (...) {
                              std::exception_ptr ep =
                                  std::current_exception();
                              // Annulation cooperative levee en exception :
                              // Cancelled, pas Failed.
                              bool wasCancel = false;
                              try {
                                  std::rethrow_exception(ep);
                              } catch (const std::future_error&) {
                                  wasCancel = false;
                              } catch (...) {
                                  std::lock_guard<std::mutex> lk(g_m);
                                  auto self = find_locked(id);
                                  wasCancel = self && self->cancel.load();
                              }
                              {
                                  std::lock_guard<std::mutex> lk(g_m);
                                  auto self = find_locked(id);
                                  if (self && !terminal(self->state)) {
                                      if (wasCancel) {
                                          self->state = State::Cancelled;
                                      } else {
                                          self->state = State::Failed;
                                          try {
                                              std::rethrow_exception(ep);
                                          } catch (const std::exception& ex) {
                                              self->status = ex.what();
                                          } catch (...) {
                                              self->status = "Echec inattendu.";
                                          }
                                      }
                                      g_version.fetch_add(1);
                                  }
                              }
                              if (!wasCancel && on_error) on_error(ep);
                          }
                      });
}

} // namespace tl::tasks
