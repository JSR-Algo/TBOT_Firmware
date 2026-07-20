#ifndef PASSIVE_RECONNECT_POLICY_H
#define PASSIVE_RECONNECT_POLICY_H

inline bool PassiveReconnectHasOwner(bool reconnect_pending, bool connect_in_flight) {
    return reconnect_pending || connect_in_flight;
}

#endif  // PASSIVE_RECONNECT_POLICY_H
