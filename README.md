# soft-patch-panel

This project has been moved to the dpdk.org site http://www.dpdk.org/browse/apps/spp/

Copy of DPDK to prototype a resource manager.

SPP is a framework for managing DPDK resources.  The first version of SPP provides for the management of DPDK ports, and assigning ports to different DPDK applications.  The framework is composed of a primary DPDK application that is responsible for resource management.  This primary application doesn’t interact with any traffic, and is used to manage creation and freeing of resources only.  A Python based management interface is provided to control the primary DPDK application to create resources, which are then to be used by secondary applications.  This management application provides a socket based interface for the primary and secondary DPDK applications to interface to the manager.  The management application utilizes OVSDB to maintain all created and assigned ports.  The goal of SPP is to easily interconnect DPDK applications together, and assign resources dynamically to these applications to build a pipeline.
