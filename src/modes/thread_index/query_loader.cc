# include "astroid.hh"
# include "db.hh"
# include "log.hh"

# include "query_loader.hh"
# include "thread_index_list_view.hh"
# include "config.hh"
# include "actions/action_manager.hh"

# include <thread>
# include <queue>
# include <mutex>
# include <functional>

# include <notmuch.h>

using std::endl;

namespace Astroid {
  QueryLoader::QueryLoader () {
    ustring sort_order = astroid->config ().get<std::string> ("thread_index.sort_order");
    if (sort_order == "newest") {
      sort = NOTMUCH_SORT_NEWEST_FIRST;
    } else if (sort_order == "oldest") {
      sort = NOTMUCH_SORT_OLDEST_FIRST;
    } else if (sort_order == "messageid") {
      sort = NOTMUCH_SORT_MESSAGE_ID;
    } else if (sort_order == "unsorted") {
      sort = NOTMUCH_SORT_UNSORTED;
    } else {
      log << error << "ti: unknown sort order, must be 'newest', 'oldest', 'messageid' or 'unsorted': " << sort_order << ", using 'newest'." << endl;
      sort = NOTMUCH_SORT_NEWEST_FIRST;
    }

    loaded_threads = 0;
    total_messages = 0;
    unread_messages = 0;
    run = false;

    queue_has_data.connect (std::bind (&QueryLoader::to_list_adder, this));

    astroid->global_actions->signal_thread_changed ().connect (
        std::bind (&QueryLoader::on_thread_changed, this, std::placeholders::_1, std::placeholders::_2));

    astroid->global_actions->signal_refreshed ().connect (
        std::bind (&QueryLoader::on_refreshed, this));
  }

  QueryLoader::~QueryLoader () {
    in_destructor = true;
    stop ();
  }

  void QueryLoader::start (ustring q) {
    query = q;
    run = true;
    loader_thread = std::thread (&QueryLoader::loader, this);
  }

  void QueryLoader::stop () {
    run = false;
    loader_thread.join ();
  }

  void QueryLoader::reload () {
    stop ();
    list_store->clear ();
    start (query);
  }

  void QueryLoader::refine_query (ustring q) {
    query = q;
    reload ();
  }

  void QueryLoader::refresh_stats (Db * db) {
    log << debug << "ql: refresh stats.." << endl;

    notmuch_query_t * query_t =  notmuch_query_create (db->nm_db, query.c_str ());
    for (ustring & t : db->excluded_tags) {
      notmuch_query_add_tag_exclude (query_t, t.c_str());
    }
    notmuch_query_set_omit_excluded (query_t, NOTMUCH_EXCLUDE_TRUE);
    /* st = */ notmuch_query_count_messages_st (query_t, &total_messages); // destructive
    notmuch_query_destroy (query_t);

    ustring unread_q_s = "(" + query + ") AND tag:unread";
    notmuch_query_t * unread_q = notmuch_query_create (db->nm_db, unread_q_s.c_str());
    for (ustring & t : db->excluded_tags) {
      notmuch_query_add_tag_exclude (unread_q, t.c_str());
    }
    notmuch_query_set_omit_excluded (unread_q, NOTMUCH_EXCLUDE_TRUE);
    /* st = */ notmuch_query_count_messages_st (unread_q, &unread_messages); // destructive
    notmuch_query_destroy (unread_q);

    if (!in_destructor) stats_ready.emit ();
  }

  void QueryLoader::loader () {
    time_t t0 = clock ();

    log << debug << "ql: load threads for query: " << query << endl;


    Db db (Db::DATABASE_READ_ONLY);

    /* set up query */
    notmuch_query_t * nmquery;
    notmuch_threads_t * threads;

    nmquery = notmuch_query_create (db.nm_db, query.c_str ());
    for (ustring & t : db.excluded_tags) {
      notmuch_query_add_tag_exclude (nmquery, t.c_str());
    }

    notmuch_query_set_omit_excluded (nmquery, NOTMUCH_EXCLUDE_TRUE);
    notmuch_query_set_sort (nmquery, sort);

    /* slow */
    /* notmuch_status_t st = */ notmuch_query_search_threads_st (nmquery, &threads);
    float diff = (clock () - t0) * 1000.0 / CLOCKS_PER_SEC;

    log << debug << "ql: query time: " << diff << " ms." << endl;

    loaded_threads = 0; // incremented in list_adder
    int i = 0;

    for (;
         run && notmuch_threads_valid (threads);
         notmuch_threads_move_to_next (threads)) {

      notmuch_thread_t  * thread;
      thread = notmuch_threads_get (threads);

      if (thread == NULL) {
        log << error << "ql: error: could not get thread." << endl;
        throw database_error ("ql: could not get thread (is NULL)");
      }

      NotmuchThread *t = new NotmuchThread (thread);

      notmuch_thread_destroy (thread);

      std::unique_lock<std::mutex> lk (to_list_m);

      to_list_store.push (refptr<NotmuchThread>(t));

      lk.unlock ();

      i++;

      if ((i % 100) == 0) {
        if (!in_destructor)
          queue_has_data.emit ();
      }
    }

    /* closing query */
    notmuch_threads_destroy (threads);
    notmuch_query_destroy (nmquery);

    refresh_stats (&db);

    log << info << "ql: loaded " << i << " threads in " << ((clock()-t0) * 1000.0 / CLOCKS_PER_SEC) << " ms." << endl;

    if (!run) {
      log << warn << "ql: stopped before finishing." << endl;
    }

    run = false;

    // catch any remaining entries
    if (!in_destructor)
      queue_has_data.emit ();
  }

  void QueryLoader::to_list_adder () {
    std::lock_guard<std::mutex> lk (to_list_m);

    while (!to_list_store.empty ()) {
      refptr<NotmuchThread> t = to_list_store.front ();
      to_list_store.pop ();

      auto iter = list_store->append ();
      Gtk::ListStore::Row row = *iter;

      row[list_store->columns.newest_date] = t->newest_date;
      row[list_store->columns.oldest_date] = t->oldest_date;
      row[list_store->columns.thread_id]   = t->thread_id;
      row[list_store->columns.thread]      = t;

      if (loaded_threads == 0) {
        if (!in_destructor)
          first_thread_ready.emit ();
      }

      loaded_threads++;

      if ((loaded_threads % 100) == 0) {
        log << debug << "ql: loaded " << loaded_threads << " threads." << endl;
      }
    }

    /* check if there are any deferred thread_updated events */
    if (!run && !thread_changed_events_waiting.empty ()) {
      Db db (Db::DATABASE_READ_ONLY);

      while (!thread_changed_events_waiting.empty ()) {
        ustring ti = thread_changed_events_waiting.front ();
        thread_changed_events_waiting.pop ();

        log << debug << "ql: running deferred thread changed event on: " << ti << endl;
        on_thread_changed (&db, ti);

      }
    }
  }

  /***************
   * signals
   **************/
  void QueryLoader::on_refreshed () {
    if (in_destructor) return;

    log << warn << "ql: got refreshed signal." << endl;
    reload ();
  }

  void QueryLoader::on_thread_changed (Db * db, ustring thread_id) {
    if (in_destructor) return;

    log << info << "ql: got changed thread signal: " << thread_id << endl;

    if (run) {
      log << warn << "ql: query is still loading, event will be deferred to later." << endl;

      thread_changed_events_waiting.push (thread_id);
      return;
    }

    /* we now have three options:
     * - a new thread has been added (unlikely)
     * - a thread has been deleted (kind of likely)
     * - a thread has been updated (most likely)
     *
     * none of them needs to affect the threads that match the query in this
     * list.
     *
     */

    time_t t0 = clock ();

    Gtk::TreePath path;
    Gtk::TreeIter fwditer;

    /* forward iterating is much faster than going backwards:
     * https://developer.gnome.org/gtkmm/3.11/classGtk_1_1TreeIter.html
     */

    bool found = false;
    fwditer = list_store->get_iter ("0");

    Gtk::ListStore::Row row;

    while (fwditer) {

      row = *fwditer;

      if (row[list_store->columns.thread_id] == thread_id) {
        found = true;
        break;
      }

      fwditer++;
    }

    /* test if thread is in the current query */
    std::lock_guard<Db> grd (*db);
    bool in_query = db->thread_in_query (query, thread_id);

    if (found) {
      /* thread has either been updated or deleted from current query */
      log << debug << "ql: updated: found thread in: " << ((clock() - t0) * 1000.0 / CLOCKS_PER_SEC) << " ms." << endl;

      if (in_query) {
        /* updated */
        log << debug << "ql: updated" << endl;
        refptr<NotmuchThread> thread = row[list_store->columns.thread];
        thread->refresh (db);
        row[list_store->columns.newest_date] = thread->newest_date;
        row[list_store->columns.oldest_date] = thread->oldest_date;

      } else {
        /* deleted */
        log << debug << "ql: deleted" << endl;
        path = list_store->get_path (fwditer);
        list_store->erase (fwditer);
      }

    } else {
      /* thread has possibly been added to the current query */
      log << debug << "ql: updated: did not find thread, time used: " << ((clock() - t0) * 1000.0 / CLOCKS_PER_SEC) << " ms." << endl;
      if (in_query) {
        log << debug << "ql: new thread for query, adding.." << endl;
        auto iter = list_store->prepend ();
        Gtk::ListStore::Row newrow = *iter;

        NotmuchThread * t;

        db->on_thread (thread_id, [&](notmuch_thread_t *nmt) {

            t = new NotmuchThread (nmt);

          });

        newrow[list_store->columns.newest_date] = t->newest_date;
        newrow[list_store->columns.oldest_date] = t->oldest_date;
        newrow[list_store->columns.thread_id]   = t->thread_id;
        newrow[list_store->columns.thread]      = Glib::RefPtr<NotmuchThread>(t);

        /* check if we should select it (if this is the only item) */
        if (list_store->children().size() == 1) {
          first_thread_ready.emit ();
        }
      }
    }

    refresh_stats (db);
  }
}
