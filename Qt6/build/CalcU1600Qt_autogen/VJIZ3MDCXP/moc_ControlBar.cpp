/****************************************************************************
** Meta object code from reading C++ file 'ControlBar.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../app/ControlBar.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ControlBar.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN10ControlBarE_t {};
} // unnamed namespace

template <> constexpr inline auto ControlBar::qt_create_metaobjectdata<qt_meta_tag_ZN10ControlBarE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "ControlBar",
        "modelSelected",
        "",
        "Model",
        "model",
        "romRevisionSelected",
        "PC1500RomRevision",
        "revision",
        "resetClicked",
        "allReset",
        "moduleSelected",
        "slot",
        "moduleNameOrEmpty",
        "nameAndSaveRequested",
        "settingsRequested",
        "openPresetRequested",
        "ce150ToggleRequested",
        "ce1600pToggleRequested"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'modelSelected'
        QtMocHelpers::SignalData<void(Model)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Signal 'romRevisionSelected'
        QtMocHelpers::SignalData<void(PC1500RomRevision)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 7 },
        }}),
        // Signal 'resetClicked'
        QtMocHelpers::SignalData<void(bool)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 9 },
        }}),
        // Signal 'moduleSelected'
        QtMocHelpers::SignalData<void(int, QString)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 11 }, { QMetaType::QString, 12 },
        }}),
        // Signal 'nameAndSaveRequested'
        QtMocHelpers::SignalData<void(int)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 11 },
        }}),
        // Signal 'settingsRequested'
        QtMocHelpers::SignalData<void()>(14, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'openPresetRequested'
        QtMocHelpers::SignalData<void()>(15, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'ce150ToggleRequested'
        QtMocHelpers::SignalData<void()>(16, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'ce1600pToggleRequested'
        QtMocHelpers::SignalData<void()>(17, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<ControlBar, qt_meta_tag_ZN10ControlBarE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject ControlBar::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10ControlBarE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10ControlBarE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN10ControlBarE_t>.metaTypes,
    nullptr
} };

void ControlBar::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<ControlBar *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->modelSelected((*reinterpret_cast<std::add_pointer_t<Model>>(_a[1]))); break;
        case 1: _t->romRevisionSelected((*reinterpret_cast<std::add_pointer_t<PC1500RomRevision>>(_a[1]))); break;
        case 2: _t->resetClicked((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 3: _t->moduleSelected((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 4: _t->nameAndSaveRequested((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 5: _t->settingsRequested(); break;
        case 6: _t->openPresetRequested(); break;
        case 7: _t->ce150ToggleRequested(); break;
        case 8: _t->ce1600pToggleRequested(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)(Model )>(_a, &ControlBar::modelSelected, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)(PC1500RomRevision )>(_a, &ControlBar::romRevisionSelected, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)(bool )>(_a, &ControlBar::resetClicked, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)(int , QString )>(_a, &ControlBar::moduleSelected, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)(int )>(_a, &ControlBar::nameAndSaveRequested, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)()>(_a, &ControlBar::settingsRequested, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)()>(_a, &ControlBar::openPresetRequested, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)()>(_a, &ControlBar::ce150ToggleRequested, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (ControlBar::*)()>(_a, &ControlBar::ce1600pToggleRequested, 8))
            return;
    }
}

const QMetaObject *ControlBar::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ControlBar::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10ControlBarE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int ControlBar::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void ControlBar::modelSelected(Model _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void ControlBar::romRevisionSelected(PC1500RomRevision _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void ControlBar::resetClicked(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void ControlBar::moduleSelected(int _t1, QString _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2);
}

// SIGNAL 4
void ControlBar::nameAndSaveRequested(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void ControlBar::settingsRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void ControlBar::openPresetRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void ControlBar::ce150ToggleRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void ControlBar::ce1600pToggleRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}
QT_WARNING_POP
