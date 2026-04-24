24th April

Today's Checklist:

    [ ] Step 1: Key Generation: Use ftok with the /tmp/trattoria_ipc_key path to generate our base keys.

    [ ] Step 2: Message Queues: Connect our client to the three message queues (Client-to-Server, Server-to-Client, and Fatigue) using System V IPC.

    [ ] Step 3: Shared Memory: Attach to the four shared memory segments (Dining Room, Kitchen, Cash Desk, and Blackboard).

    [ ] Step 4: Semaphores: Attach to the semaphore set so we can safely lock the blackboard later.

    [ ] Step 5: The Handshake: Write the logic to construct and send the HELLO message containing our student IDs. Then, wait to successfully receive the WELCOME message back from the server.

    [ ] Step 6: Clean Teardown: Write the cleanup function. We need to make sure we safely detach from all shared memory and queues when the program exits so we don't leave zombie resources that break the server on our next run.
